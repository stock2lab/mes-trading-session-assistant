#pragma once

#include <time.h>

enum MesDayType : unsigned char {
  MES_DAY_REGULAR,
  MES_DAY_EARLY_CLOSE,
  MES_DAY_CLOSED
};

struct MesDaySchedule {
  MesDayType type;
  int close_minutes_et;
  const char *name;
};

MesDaySchedule mes_day_schedule(const tm &new_york);

