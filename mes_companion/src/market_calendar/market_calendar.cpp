#include "market_calendar.h"

namespace {
struct CalendarDate {
  short year;
  unsigned char month;
  unsigned char day;
  MesDayType type;
  short close_minutes_et;
  const char *name;
};

// CME equity-index holiday daytime schedules relevant to the user's
// 09:30-14:00 ET discipline window. CME may revise holiday hours near the date.
const CalendarDate kCalendar[] = {
  {2026, 1, 1, MES_DAY_CLOSED, 0, "NEW YEAR'S DAY"},
  {2026, 1, 19, MES_DAY_EARLY_CLOSE, 13 * 60, "MLK DAY"},
  {2026, 2, 16, MES_DAY_EARLY_CLOSE, 13 * 60, "PRESIDENTS DAY"},
  {2026, 4, 3, MES_DAY_CLOSED, 0, "GOOD FRIDAY"},
  {2026, 5, 25, MES_DAY_EARLY_CLOSE, 13 * 60, "MEMORIAL DAY"},
  {2026, 6, 19, MES_DAY_EARLY_CLOSE, 13 * 60, "JUNETEENTH"},
  {2026, 7, 3, MES_DAY_EARLY_CLOSE, 13 * 60, "INDEPENDENCE DAY"},
  {2026, 9, 7, MES_DAY_EARLY_CLOSE, 13 * 60, "LABOR DAY"},
  {2026, 11, 26, MES_DAY_EARLY_CLOSE, 13 * 60, "THANKSGIVING"},
  {2026, 11, 27, MES_DAY_EARLY_CLOSE, 13 * 60, "THANKSGIVING FRIDAY"},
  {2026, 12, 24, MES_DAY_EARLY_CLOSE, 13 * 60, "CHRISTMAS EVE"},
  {2026, 12, 25, MES_DAY_CLOSED, 0, "CHRISTMAS DAY"},
  {2027, 1, 1, MES_DAY_CLOSED, 0, "NEW YEAR'S DAY"},
};
}

MesDaySchedule mes_day_schedule(const tm &new_york) {
  if (new_york.tm_wday == 0 || new_york.tm_wday == 6) {
    return {MES_DAY_CLOSED, 0, "WEEKEND"};
  }
  const int year = new_york.tm_year + 1900;
  const int month = new_york.tm_mon + 1;
  for (const CalendarDate &entry : kCalendar) {
    if (entry.year == year && entry.month == month && entry.day == new_york.tm_mday) {
      return {entry.type, entry.close_minutes_et, entry.name};
    }
  }
  return {MES_DAY_REGULAR, 16 * 60, "REGULAR SESSION"};
}
