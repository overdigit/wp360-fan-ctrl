#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <gpiod.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#ifndef	CONSUMER
#define	CONSUMER "wp360-fan-governor"
#endif

#if DEBUG
#define FAN_PERIOD  60
#else
#define FAN_PERIOD  3600
#endif

#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char *)


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
  return temp;
}


int main(void)
{
  char *chipname = "gpiochip0";
  struct gpiod_chip *chip;
  struct gpiod_line *line[2];
  int pin_fan[2] = {26, 27};
  int ret;
  unsigned char return_code = 0;
  time_t unix_time = time(NULL);
  srand(unix_time);
  int fan_low, fan_high, fan_time = rand() % FAN_PERIOD;
  int vcgencmd_err = 0;
  float temp_deg;
#if DEBUG
  char log_path[256];
  char date_fmt[256];
  strftime(log_path, 256, "/var/log/wp360-fan-control/%Y%m%d-%H%M%S.log", gmtime(&unix_time));
  FILE *log = fopen(log_path,"w");
  if (!log)
  {
    return 252;
  }
  fprintf(log,"Time,Temperature,fan_low,fan_high,fan0,fan1\n");
#endif

  chip = gpiod_chip_open_by_name(chipname);
  if (!chip)
  {
    perror("Open chip failed\n");
    return_code = 255;
    goto end;
  }

  line[0] = gpiod_chip_get_line(chip, pin_fan[0]);
  line[1] = gpiod_chip_get_line(chip, pin_fan[1]);
  if (!(line[0] && line[1]))
  {
    perror("Get line failed\n");
    return_code = 254;
    goto close_chip;
  }

  ret = gpiod_line_request_output(line[0], CONSUMER, 0) || gpiod_line_request_output(line[1], CONSUMER, 0);
  if (ret < 0)
  {
    perror("Request line as output failed\n");
    return_code = 253;
    goto release_line;
  }

  while (true)
  {
    temp_deg = vcgencmd_measure_temp(&vcgencmd_err);
    if (vcgencmd_err)
    {
      return_code = vcgencmd_err;
      break;
    }

    fan_low  = temp_deg > 65.0 || (fan_low  && temp_deg > 60.0);
    fan_high = temp_deg > 87.0 || (fan_high && temp_deg > 83.0);

    gpiod_line_set_value(line[fan_time <= FAN_PERIOD/2],  fan_low);
#if DEBUG
    unix_time = time(NULL);
    strftime(date_fmt, 256, "%Y-%m-%d %H:%M:%S", gmtime(&unix_time));
    fprintf(stderr, "%s, Temp: %3.1f, fan_low: %d, fan_high: %d, fan0: %d, fan1: %d\n", date_fmt, temp_deg, fan_low, fan_high, gpiod_line_get_value(line[0]), gpiod_line_get_value(line[1]));
    fprintf(log, "%s,%3.1f,%d,%d,%d,%d\n", date_fmt, temp_deg, fan_low, fan_high, gpiod_line_get_value(line[0]), gpiod_line_get_value(line[1]));
    fflush(log);
#endif
    sleep(1);
    gpiod_line_set_value(line[fan_time > FAN_PERIOD/2], fan_high);
    if (fan_low && !fan_high) {if(++fan_time > FAN_PERIOD-1) fan_time = 0;}
  }
#if DEBUG
  fclose(log);
#endif
release_line:
  gpiod_line_release(line[0]);
  gpiod_line_release(line[1]);
close_chip:
  gpiod_chip_close(chip);
end:
  return return_code;
}
