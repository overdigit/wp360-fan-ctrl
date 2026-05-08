#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <gpiod.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>

#ifndef	CONSUMER
#define	CONSUMER "wp360-fan-governor"
#endif

#define FAN_PERIOD  3600
#if DEBUG
#define FAN_PERIOD  60
#endif

#define PIN_FAN0  26
#define PIN_FAN1  27
#define PIN_PWM   6

#define FAN_LOW_TEMP_ENGAGE       55.0
#define FAN_LOW_TEMP_DISENGAGE    50.0
#define FAN_HIGH_TEMP_ENGAGE      999.0 // Absurdly high to disable
#define FAN_HIGH_TEMP_DISENGAGE   999.0 // Absurdly high to disable
#define FAN_HIGH_PWM_ENGAGE       550
#define FAN_HIGH_PWM_DISENGAGE    375
#define PWM_MIN_DUTY              200
#define PWM_MAX_DUTY              900
#define PWM_STEP                  25
#define PWM_RAMP_TEMP             60.0
#define PWM_SLOW_TEMP             55.0

#define FIRMWARE_PWM_Y            26
#define FIRMWARE_PWM_M            05
#define FIRMWARE_PWM_D            07

#define PWM_OFFLOAD(x)    { if (pwm_offload != 0) { x } }
#define NO_PWM_OFFLOAD(x) { if (pwm_offload != 1) { x } }

#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)

float temp_deg;
int duty_cycle = PWM_MIN_DUTY;
pthread_mutex_t temp_mutex;
pthread_mutex_t line_mutex;
pthread_mutex_t duty_mutex;

int check_firmware_date(int y, int m, int d)
{
  if (y < FIRMWARE_PWM_Y)
  {
    return 0;
  }
  else if (y > FIRMWARE_PWM_Y)
  {
    return 1;
  }
  if (m < FIRMWARE_PWM_M)
  {
    return 0;
  }
  else if (m > FIRMWARE_PWM_M)
  {
    return 1;
  }
  if (d < FIRMWARE_PWM_D)
  {
    return 0;
  }
  return 1;
}

int check_pwm_offload(void)
{
  int y, m, d, n;
  char firmware_release_str[10];
  FILE *firmware_release_f = fopen("/sys/kernel/wp360-pmuc/firmware_release", "r");

  if (firmware_release_f == NULL)
  {
    return -1;
  }

  if (fgets(firmware_release_str, 10, firmware_release_f) == NULL)
  {
    fclose(firmware_release_f);
    return -1;
  }

  if (sscanf(firmware_release_str, "%d-%d-%d", &y, &m, &d) != 3)
  {
    fclose(firmware_release_f);
    return -1;
  }

  n = check_firmware_date(y, m, d);
  fclose(firmware_release_f);
  return n;
}

float vcgencmd_measure_temp(int *err)
{
  unsigned p[64 + 7] = {72*sizeof(unsigned), 0, 0x0030080, 256, 0, 0};
  memcpy(p + 6,"measure_temp", 13);

  int mb = open("/dev/vcio", 0);
  if (mb < 0)
  {
    *err = 1;
    return 0;
  }

  int ret = ioctl(mb,IOCTL_MBOX_PROPERTY, p);
  if (ret < 0)
  {
    *err = 2;
    return 0;
  }

  close(mb);
  char* temp_start = (char*)p + 29;
  char* temp_end;
  float temp = strtof(temp_start, &temp_end);

  if (temp_end == temp_start)
  {
    *err = 3;
    return 0;
  }
  temp_deg = temp;
}

struct pwm_thread_arg
{
  struct gpiod_line_request *line;
};

void *pwm_thread(void* args)
{
  struct sched_param param;
  param.sched_priority = (sched_get_priority_min(SCHED_RR) * 9 + sched_get_priority_max(SCHED_RR)) / 10;
  sched_setscheduler(0, SCHED_RR, &param);

  struct pwm_thread_arg *arg = (struct pwm_thread_arg *) args;
  struct gpiod_line_request *line = arg->line;
  struct timespec time;
  clock_gettime(CLOCK_REALTIME, &time);
  int us_on = 300;
  while(1)
  { 
    pthread_mutex_lock(&line_mutex);
    gpiod_line_request_set_value(line, PIN_PWM, GPIOD_LINE_VALUE_ACTIVE);
    pthread_mutex_unlock(&line_mutex);
    pthread_mutex_lock(&duty_mutex);
    us_on = duty_cycle;
    pthread_mutex_unlock(&duty_mutex);
    time.tv_nsec += us_on * 1000;
    if (time.tv_nsec > 999999999) {
      time.tv_sec += 1;
      time.tv_nsec -= 1000000000;
    }
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &time, NULL);
    pthread_mutex_lock(&line_mutex);
    gpiod_line_request_set_value(line, PIN_PWM, GPIOD_LINE_VALUE_INACTIVE);
    pthread_mutex_unlock(&line_mutex);
    time.tv_nsec += 1000000 - us_on * 1000;
    if (time.tv_nsec > 999999999) {
      time.tv_sec += 1;
      time.tv_nsec -= 1000000000;
    }
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &time, NULL);
  }
}

int main(void)
{
  char *chipname = "/dev/gpiochip0";
  struct gpiod_chip *chip;
  struct gpiod_line_request *line;
  struct gpiod_line_config *config;
  struct gpiod_line_settings *settings;
  struct gpiod_request_config *req_cfg;
  int pin_fan[3] = {PIN_FAN0, PIN_FAN1, PIN_PWM};
  int ret;
  unsigned char return_code = 0;
  time_t unix_time = time(NULL);
  srand(unix_time);
  int fan_low, fan_high, fan_time = rand() % FAN_PERIOD;
  int vcgencmd_err = 0;
  int pwm_duty_cycle = PWM_MIN_DUTY;
  pthread_t pwm_thread_tid;
  struct pwm_thread_arg pwm_args;
  int pwm_offload = check_pwm_offload();
  FILE *pwm_offload_f;
  NO_PWM_OFFLOAD(pthread_mutex_init(&line_mutex, NULL););
  NO_PWM_OFFLOAD(pthread_mutex_init(&temp_mutex, NULL););
#if DEBUG
  char log_path[256];
  char date_fmt[256];
  enum gpiod_line_value states[3];
  strftime(log_path, 256, "/var/log/wp360-fan-ctrl/%Y%m%d-%H%M%S.log", gmtime(&unix_time));
  FILE *log = fopen(log_path,"w");
  if (!log)
  {
    return 252;
  }
  fprintf(log,"Time,Temperature,fan_low,fan_high,fan0,fan1,duty_cycle\n");
#endif

  chip = gpiod_chip_open(chipname);
  if (!chip)
  {
    perror("Open chip failed\n");
    return_code = 255;
    goto end;
  }

  settings = gpiod_line_settings_new();
  if (settings == NULL)
  {
    perror("Could not initialize line settings\n");
    return_code = 254;
    goto close_chip;
  }

  if (
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) ||
    gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_PUSH_PULL) ||
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE)
  )
  {
    perror("Could not set line settings\n");
    return_code = 253;
    goto release_settings;
  }

  config = gpiod_line_config_new();
  if (config == NULL)
  {
    perror("Could not initialize line configuration\n");
    return_code = 252;
    goto release_settings;
  }
  if (
    gpiod_line_config_add_line_settings(config, pin_fan, (pwm_offload == 1) ? 2 : 3, settings)
  )
  {
    perror("Could not set line configuration\n");
    return_code = 251;
    goto release_config;
  }

  req_cfg = gpiod_request_config_new();
  if (req_cfg == NULL)
  {
    perror("Could not initialize request configuration\n");
    return_code = 250;
    goto release_config;
  }
  gpiod_request_config_set_consumer(req_cfg, CONSUMER);

  line = gpiod_chip_request_lines(chip, req_cfg, config);
  if (line == NULL)
  {
    perror("Could not request line\n");
    return_code = 248;
    goto release_req_cfg;
  }
  pwm_args.line = line;
  NO_PWM_OFFLOAD(pthread_create(&pwm_thread_tid, NULL, pwm_thread, (void*) &pwm_args););
  PWM_OFFLOAD(pwm_offload_f = fopen("/sys/kernel/wp360-pmuc/fan_voltage", "w"); if (pwm_offload_f == NULL) { perror("Could not open fan driver file"); return_code = 246; goto release_line; });
  while (true)
  {
    vcgencmd_measure_temp(&vcgencmd_err);
    if (vcgencmd_err)
    {
      return_code = vcgencmd_err;
      break;
    }

    fan_low  = temp_deg > FAN_LOW_TEMP_ENGAGE || (fan_low  && temp_deg > FAN_LOW_TEMP_DISENGAGE);
    fan_high = temp_deg > FAN_HIGH_TEMP_ENGAGE || (fan_high && temp_deg > FAN_HIGH_TEMP_DISENGAGE) ||
               pwm_duty_cycle > FAN_HIGH_PWM_ENGAGE || (fan_high && pwm_duty_cycle > FAN_HIGH_PWM_DISENGAGE);

    if (!fan_low)
    {
      pwm_duty_cycle = PWM_MIN_DUTY;
    } else {
      if (temp_deg > PWM_RAMP_TEMP)
      {
        pwm_duty_cycle += PWM_STEP;
        if (pwm_duty_cycle > PWM_MAX_DUTY)
        {
          pwm_duty_cycle = PWM_MAX_DUTY;
        }
      }
      else if (temp_deg < PWM_SLOW_TEMP)
      {
        pwm_duty_cycle -= PWM_STEP;
        if (pwm_duty_cycle < PWM_MIN_DUTY)
        {
          pwm_duty_cycle = PWM_MIN_DUTY;
        }
      }
    }
    NO_PWM_OFFLOAD(pthread_mutex_lock(&duty_mutex););
    duty_cycle = pwm_duty_cycle;
    PWM_OFFLOAD(fprintf(pwm_offload_f, "%d", duty_cycle / 10); fflush(pwm_offload_f););
    NO_PWM_OFFLOAD(pthread_mutex_unlock(&duty_mutex););

    NO_PWM_OFFLOAD(pthread_mutex_lock(&line_mutex););
    ret = gpiod_line_request_set_value(line, pin_fan[fan_time <= FAN_PERIOD/2], fan_low ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
    NO_PWM_OFFLOAD(pthread_mutex_unlock(&line_mutex););
    if (ret)
    {
      perror("Could not set line value\n");
      return_code = 247;
      goto release_line;
    }
#if DEBUG
    unix_time = time(NULL);
    strftime(date_fmt, 256, "%Y-%m-%d %H:%M:%S", gmtime(&unix_time));
    NO_PWM_OFFLOAD(pthread_mutex_lock(&line_mutex););
    gpiod_line_request_get_values(line, states);
    NO_PWM_OFFLOAD(pthread_mutex_unlock(&line_mutex););
    fprintf(stderr, "%s, Temp: %3.1f, fan_low: %d, fan_high: %d, fan0: %d, fan1: %d, duty cycle: %d%%\n", 
        date_fmt,
        temp_deg,
        fan_low,
        fan_high,
        states[0] == GPIOD_LINE_VALUE_ACTIVE,
        states[1] == GPIOD_LINE_VALUE_ACTIVE,
        pwm_duty_cycle / 10
    );
    fprintf(log, "%s,%3.1f,%d,%d,%d,%d,%d\n", date_fmt, temp_deg, fan_low, fan_high, states[0] == GPIOD_LINE_VALUE_ACTIVE, states[1] == GPIOD_LINE_VALUE_ACTIVE, pwm_duty_cycle / 10);
    fflush(log);
#endif
    sleep(1);
    NO_PWM_OFFLOAD(pthread_mutex_lock(&line_mutex););
    ret = gpiod_line_request_set_value(line, pin_fan[fan_time > FAN_PERIOD/2], fan_high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
    NO_PWM_OFFLOAD(pthread_mutex_unlock(&line_mutex););
    if (ret)
    {
      perror("Could not set line value\n");
      return_code = 247;
      goto release_line;
    }
    if (fan_low && !fan_high) {if(++fan_time > FAN_PERIOD-1) fan_time = 0;}
  }
#if DEBUG
  fclose(log);
#endif
release_line:
  gpiod_line_request_release(line);
release_req_cfg:
  gpiod_request_config_free(req_cfg);
release_config:
  gpiod_line_config_free(config);
release_settings:
  gpiod_line_settings_free(settings);
close_chip:
  gpiod_chip_close(chip);
end:
  return return_code;
}
