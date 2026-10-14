#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_sntp.h>
#include "src/lvgl_port/lvgl_port.h"
#include "src/rtc_pcf85063a/rtc_pcf85063a.h"
#include "src/speaker_microphone/codec_dev.h"
#include "src/market_calendar/market_calendar.h"

namespace {
constexpr const char *FIRMWARE_VERSION="v0.6.0-test";
constexpr const char *SETUP_AP_SSID="TradingSession-Setup";
constexpr const char *SETUP_AP_PASS="tradeclock";
constexpr uint32_t BG=0x0B1018,PANEL=0x151D2A,TEXT=0xF2F5F8,MUTED=0x8290A3;
constexpr uint32_t ACCENT=0x2F80ED,WARN=0xF2C94C,DANGER=0xEB5757;
const uint8_t intervals[]={1,3,5,15};
uint8_t interval_index=2;
lv_obj_t *countdown_label,*interval_label,*bar_number_label,*bar_closed_label,*beijing_label,*new_york_label,*new_york_zone_label,*session_label,*progress_bar;
lv_obj_t *sound_label;
lv_obj_t *timer_screen,*journal_screen,*journal_summary,*journal_status,*position_label,*journal_history;
lv_obj_t *home_trades_button,*home_trades_label;
lv_obj_t *reset_label,*undo_label,*long_button,*short_button,*cancel_button,*result_buttons[7];
lv_obj_t *custom_panel,*custom_value_label;
lv_obj_t *discipline_label,*session_lock_panel,*session_lock_title,*session_lock_reason,*override_button,*override_button_label;
lv_obj_t *network_status_label;
int custom_half_r=5;
time_t undo_armed_until=0;
uint32_t override_press_started=0;
bool after_hours_override=false;
int last_1300_alert_day=0;
bool sound_enabled=false,codec_ready=false;
time_t last_chime_epoch=0;
time_t reset_armed_until=0;
enum NetworkState{NET_CONNECTING,NET_PORTAL,NET_ONLINE};
NetworkState network_state=NET_CONNECTING;
WebServer web_server(80);
DNSServer dns_server;
String wifi_ssids[3],wifi_passwords[3];
uint8_t wifi_count=0,wifi_index=0;
uint32_t wifi_attempt_started=0,next_wifi_retry=0;
bool portal_active=false,portal_forced=false,web_started=false,web_listening=false,time_synced=false,ntp_requested=false,time_check_required=false;
uint32_t network_hold_started=0;
time_t last_rtc_network_sync=0,pending_ntp_epoch=0;
bool test_mode=false;
time_t test_epoch_base=0;
uint32_t test_millis_base=0;
struct TradeState{
  int day=0;uint8_t count=0;int8_t half_r=0;uint8_t losses=0;bool open=false;char side='-';
  uint32_t entry=0;uint32_t cooldown=0;char history_side[5]{};
  int8_t history_half_r[5]{};uint32_t history_time[5]{};
}trade;

int build_month(const char *s){static const char *a="JanFebMarAprMayJunJulAugSepOctNovDec";const char *p=strstr(a,s);return p?((p-a)/3)+1:1;}
int weekday(int y,int m,int d){if(m<3){m+=12;--y;}int k=y%100,j=y/100;return ((d+(13*(m+1))/5+k+k/4+j/4+5*j)%7+6)%7;}
void initialise_rtc_once(){
  Preferences p;p.begin("mes-clock",false);datetime_t n{};PCF85063A_Read_now(&n);
  bool ok=n.year>=2024&&n.year<=2069&&n.month>=1&&n.month<=12&&n.day>=1&&n.day<=31&&n.hour<=23&&n.min<=59&&n.sec<=59;
  char mon[4]{};int d=1,y=2026,h=0,m=0,s=0;
  sscanf(__DATE__,"%3s %d %d",mon,&d,&y);sscanf(__TIME__,"%d:%d:%d",&h,&m,&s);int mo=build_month(mon);
  int rtc_date=ok?((int)n.year*10000+n.month*100+n.day):0,build_date=y*10000+mo*100+d;
  if(!p.getBool("initialised",false)||!ok||rtc_date<build_date){
    datetime_t t{(UWORD)y,(UBYTE)mo,(UBYTE)d,(UBYTE)weekday(y,mo,d),(UBYTE)h,(UBYTE)m,(UBYTE)s};
    PCF85063A_Set_All(t);p.putBool("initialised",true);
  }p.end();
}
String html_escape(const String &value){
  String out;out.reserve(value.length()+8);
  for(size_t i=0;i<value.length();i++){char c=value[i];if(c=='&')out+="&amp;";else if(c=='<')out+="&lt;";else if(c=='>')out+="&gt;";else if(c=='\"')out+="&quot;";else out+=c;}
  return out;
}
void load_wifi_networks(){
  wifi_count=0;Preferences p;p.begin("mes-wifi",true);
  for(int i=0;i<3;i++){String sk="ssid"+String(i),pk="pass"+String(i);String s=p.getString(sk.c_str(),"");
    if(s.length()){wifi_ssids[wifi_count]=s;wifi_passwords[wifi_count]=p.getString(pk.c_str(),"");wifi_count++;}
  }p.end();
}
void save_wifi_network(const String &ssid,const String &password){
  if(!ssid.length())return;Preferences p;p.begin("mes-wifi",false);int slot=-1,empty=-1;
  for(int i=0;i<3;i++){String sk="ssid"+String(i),saved=p.getString(sk.c_str(),"");if(saved==ssid)slot=i;if(!saved.length()&&empty<0)empty=i;}
  if(slot<0)slot=empty;
  if(slot<0){
    for(int i=0;i<2;i++){String next_s=p.getString(("ssid"+String(i+1)).c_str(),"");String next_p=p.getString(("pass"+String(i+1)).c_str(),"");
      p.putString(("ssid"+String(i)).c_str(),next_s);p.putString(("pass"+String(i)).c_str(),next_p);}
    slot=2;
  }
  p.putString(("ssid"+String(slot)).c_str(),ssid);p.putString(("pass"+String(slot)).c_str(),password);p.end();load_wifi_networks();
}
String wifi_setup_page(){
  String page=F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Trading Session Wi-Fi</title><style>body{font-family:Arial;background:#0b1018;color:#f2f5f8;max-width:520px;margin:32px auto;padding:20px}div{background:#151d2a;padding:24px;border-radius:16px}select,input,button{box-sizing:border-box;width:100%;padding:14px;margin:8px 0;border-radius:9px;border:1px solid #334155;font-size:17px}button{background:#2f80ed;color:white;border:0}small{color:#8290a3}</style></head><body><div><h2>Trading Session Wi-Fi</h2><p>Select a network and enter its password.</p><form method='post' action='/save'><select name='ssid'>");
  int found=WiFi.scanNetworks();
  for(int i=0;i<found;i++){String s=html_escape(WiFi.SSID(i));page+="<option value=\""+s+"\">"+s+" ("+String(WiFi.RSSI(i))+" dBm)</option>";}
  WiFi.scanDelete();
  page+=F("</select><input name='password' type='password' maxlength='64' placeholder='Wi-Fi password'><button type='submit'>SAVE AND CONNECT</button></form><small>Up to three networks are stored. Core timer functions continue offline.</small></div></body></html>");
  return page;
}
void connect_wifi_index(uint8_t index){
  if(index>=wifi_count)return;portal_forced=false;wifi_index=index;network_state=NET_CONNECTING;wifi_attempt_started=millis();
  WiFi.mode(portal_active?WIFI_AP_STA:WIFI_STA);
  if(!web_listening){web_server.begin();web_listening=true;}
  WiFi.disconnect();WiFi.begin(wifi_ssids[index].c_str(),wifi_passwords[index].c_str());
}
void setup_web(){
  if(web_started)return;
  web_server.on("/",HTTP_GET,[](){web_server.send(200,"text/html; charset=utf-8",wifi_setup_page());});
  web_server.on("/save",HTTP_POST,[](){
    String ssid=web_server.arg("ssid"),password=web_server.arg("password");ssid.trim();
    if(!ssid.length()){web_server.send(400,"text/plain","SSID is required");return;}
    save_wifi_network(ssid,password);
    web_server.send(200,"text/html; charset=utf-8","<html><body style='font-family:Arial'><h2>Saved</h2><p>The display is connecting. You may close this page.</p></body></html>");
    uint8_t target=0;for(uint8_t i=0;i<wifi_count;i++)if(wifi_ssids[i]==ssid)target=i;connect_wifi_index(target);
  });
  web_server.on("/generate_204",HTTP_GET,[](){web_server.sendHeader("Location","/",true);web_server.send(302,"text/plain","");});
  web_server.on("/hotspot-detect.html",HTTP_GET,[](){web_server.send(200,"text/html",wifi_setup_page());});
  web_server.onNotFound([](){web_server.sendHeader("Location","/",true);web_server.send(302,"text/plain","");});
  web_started=true;
}
void start_setup_portal(){
  if(!portal_active){WiFi.mode(WIFI_AP_STA);bool ap_ok=WiFi.softAP(SETUP_AP_SSID,SETUP_AP_PASS);dns_server.start(53,"*",WiFi.softAPIP());portal_active=true;
    Serial.printf("WIFI_AP:%s IP:%s\n",ap_ok?"OK":"FAIL",WiFi.softAPIP().toString().c_str());}
  if(!web_listening){web_server.begin();web_listening=true;}
  setup_web();network_state=NET_PORTAL;next_wifi_retry=millis()+30000;
}
void begin_wifi(){
  WiFi.persistent(false);WiFi.setAutoReconnect(false);WiFi.setSleep(false);load_wifi_networks();setup_web();
  Serial.printf("WIFI_SAVED:%u\n",wifi_count);
  if(wifi_count)connect_wifi_index(0);else start_setup_portal();
}
time_t current_epoch(datetime_t *out=nullptr);
void write_rtc_from_network(time_t epoch,bool force=false){
  time_t rtc_epoch=current_epoch();
  if(!force&&rtc_epoch>1700000000&&labs((long)(epoch-rtc_epoch))>600){
    pending_ntp_epoch=epoch;time_check_required=true;ntp_requested=false;
    Serial.printf("TIME_CHECK: RTC differs from NTP by %ld seconds\n",(long)(epoch-rtc_epoch));
    return;
  }
  time_t local_epoch=epoch+8*3600;tm bj{};gmtime_r(&local_epoch,&bj);
  datetime_t t{(UWORD)(bj.tm_year+1900),(UBYTE)(bj.tm_mon+1),(UBYTE)bj.tm_mday,(UBYTE)bj.tm_wday,(UBYTE)bj.tm_hour,(UBYTE)bj.tm_min,(UBYTE)bj.tm_sec};
  PCF85063A_Set_All(t);last_rtc_network_sync=epoch;pending_ntp_epoch=0;time_synced=true;time_check_required=false;
}
void service_wifi(){
  if(web_started)web_server.handleClient();if(portal_active)dns_server.processNextRequest();
  wl_status_t status=WiFi.status();uint32_t now_ms=millis();
  if(portal_forced){network_state=NET_PORTAL;return;}
  if(status==WL_CONNECTED){
    if(network_state!=NET_ONLINE){
      network_state=NET_ONLINE;
      if(portal_active){dns_server.stop();WiFi.softAPdisconnect(true);portal_active=false;WiFi.mode(WIFI_STA);}
      configTime(0,0,"pool.ntp.org","time.cloudflare.com","time.google.com");ntp_requested=true;
    }
    if(!test_mode&&ntp_requested&&sntp_get_sync_status()==SNTP_SYNC_STATUS_COMPLETED){
      time_t epoch=time(nullptr);if(epoch>1700000000&&(last_rtc_network_sync==0||epoch-last_rtc_network_sync>=21600))write_rtc_from_network(epoch);
    }
    return;
  }
  if(network_state==NET_ONLINE){time_synced=false;ntp_requested=false;if(wifi_count)connect_wifi_index(0);else start_setup_portal();return;}
  if(network_state==NET_CONNECTING&&now_ms-wifi_attempt_started>=8000){
    if(wifi_index+1<wifi_count)connect_wifi_index(wifi_index+1);else start_setup_portal();
  }else if(network_state==NET_PORTAL&&wifi_count&&now_ms>=next_wifi_retry){
    connect_wifi_index(0);next_wifi_retry=now_ms+30000;
  }
}
void network_setup_hold(lv_event_t *e){
  lv_event_code_t code=lv_event_get_code(e);
  if(code==LV_EVENT_PRESSED)network_hold_started=millis();
  else if(code==LV_EVENT_PRESSING&&network_hold_started&&millis()-network_hold_started>=5000){
    network_hold_started=0;
    if(time_check_required&&pending_ntp_epoch>1700000000){
      time_t confirmed_epoch=time(nullptr);
      write_rtc_from_network(confirmed_epoch>1700000000?confirmed_epoch:pending_ntp_epoch,true);
    }
    else{portal_forced=true;start_setup_portal();}
  }else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST)network_hold_started=0;
}
time_t beijing_epoch(const datetime_t &n){
  setenv("TZ","CST-8",1);tzset();tm t{};
  t.tm_year=n.year-1900;t.tm_mon=n.month-1;t.tm_mday=n.day;t.tm_hour=n.hour;t.tm_min=n.min;t.tm_sec=n.sec;t.tm_isdst=-1;
  return mktime(&t);
}
time_t current_epoch(datetime_t *out){
  if(test_mode){
    time_t epoch=test_epoch_base+(millis()-test_millis_base)/1000;
    if(out){time_t local_epoch=epoch+8*3600;tm bj{};gmtime_r(&local_epoch,&bj);
      *out=datetime_t{(UWORD)(bj.tm_year+1900),(UBYTE)(bj.tm_mon+1),(UBYTE)bj.tm_mday,(UBYTE)bj.tm_wday,(UBYTE)bj.tm_hour,(UBYTE)bj.tm_min,(UBYTE)bj.tm_sec};}
    return epoch;
  }
  datetime_t n{};PCF85063A_Read_now(&n);if(out)*out=n;return beijing_epoch(n);
}
void new_york_time(time_t epoch,tm &ny){
  setenv("TZ","EST5EDT,M3.2.0/2,M11.1.0/2",1);tzset();localtime_r(&epoch,&ny);
}
int new_york_day_key(time_t epoch){tm ny{};new_york_time(epoch,ny);return (ny.tm_year+1900)*10000+(ny.tm_mon+1)*100+ny.tm_mday;}
int new_york_minutes(time_t epoch){tm ny{};new_york_time(epoch,ny);return ny.tm_hour*60+ny.tm_min;}
bool new_york_weekday(time_t epoch){tm ny{};new_york_time(epoch,ny);return ny.tm_wday>=1&&ny.tm_wday<=5;}
bool session_allows_entry(time_t now){
  tm ny{};new_york_time(now,ny);MesDaySchedule schedule=mes_day_schedule(ny);
  if(schedule.type==MES_DAY_CLOSED)return false;int minutes=ny.tm_hour*60+ny.tm_min;
  if(schedule.type==MES_DAY_EARLY_CLOSE)return minutes>=570&&minutes<schedule.close_minutes_et;
  return (minutes>=570&&minutes<840)||after_hours_override;
}
void save_trade(){
  if(test_mode)return;
  Preferences p;p.begin("mes-journal",false);
  p.putInt("day",trade.day);p.putUChar("count",trade.count);p.putChar("halfR",trade.half_r);
  p.putUChar("losses",trade.losses);p.putBool("open",trade.open);p.putChar("side",trade.side);
  p.putUInt("entry",trade.entry);p.putUInt("cooldown",trade.cooldown);
  p.putBytes("histSide",trade.history_side,sizeof(trade.history_side));
  p.putBytes("histR",trade.history_half_r,sizeof(trade.history_half_r));
  p.putBytes("histTime",trade.history_time,sizeof(trade.history_time));p.end();
}
void load_trade(){
  Preferences p;p.begin("mes-journal",true);
  trade.day=p.getInt("day",0);trade.count=p.getUChar("count",0);trade.half_r=p.getChar("halfR",0);
  trade.losses=p.getUChar("losses",0);trade.open=p.getBool("open",false);trade.side=p.getChar("side",'-');
  trade.entry=p.getUInt("entry",0);trade.cooldown=p.getUInt("cooldown",0);
  if(p.getBytesLength("histSide")==sizeof(trade.history_side))p.getBytes("histSide",trade.history_side,sizeof(trade.history_side));
  if(p.getBytesLength("histR")==sizeof(trade.history_half_r))p.getBytes("histR",trade.history_half_r,sizeof(trade.history_half_r));
  if(p.getBytesLength("histTime")==sizeof(trade.history_time))p.getBytes("histTime",trade.history_time,sizeof(trade.history_time));p.end();
  time_t now=current_epoch();if(trade.day!=new_york_day_key(now)){trade=TradeState{};trade.day=new_york_day_key(now);save_trade();}
}
bool trading_locked(time_t now){return trade.count>=5||trade.half_r<=-4||(!trade.open&&now<trade.cooldown)||!session_allows_entry(now);}
void refresh_journal(lv_timer_t *){
  if(!journal_summary)return;datetime_t n{};time_t now=current_epoch(&n);
  if(reset_label&&reset_armed_until&&now>reset_armed_until){reset_armed_until=0;lv_label_set_text(reset_label,"RESET");}
  if(undo_label&&undo_armed_until&&now>undo_armed_until){undo_armed_until=0;lv_label_set_text(undo_label,"UNDO");}
  int ny_day=new_york_day_key(now),ny_minutes=new_york_minutes(now);tm ny{};new_york_time(now,ny);
  if(trade.day!=ny_day){trade=TradeState{};trade.day=ny_day;after_hours_override=false;save_trade();}
  if(home_trades_button&&home_trades_label){
    if(trade.open){
      uint32_t held=now>trade.entry?now-trade.entry:0;bool reminder=held>=1800;
      lv_label_set_text_fmt(home_trades_label,"%s %02d:%02d",trade.side=='L'?"LONG":"SHORT",held/60,held%60);
      lv_obj_set_style_bg_color(home_trades_button,lv_color_hex(reminder?WARN:ACCENT),LV_PART_MAIN);
      lv_obj_set_style_text_color(home_trades_label,lv_color_hex(reminder?BG:TEXT),LV_PART_MAIN);
    }else{
      lv_label_set_text(home_trades_label,"TRADES");
      lv_obj_set_style_bg_color(home_trades_button,lv_color_hex(ACCENT),LV_PART_MAIN);
      lv_obj_set_style_text_color(home_trades_label,lv_color_hex(TEXT),LV_PART_MAIN);
    }
  }
  char r[16];snprintf(r,sizeof(r),"%+.1fR",trade.half_r/2.0f);
  lv_label_set_text_fmt(journal_summary,"TRADES  %d / 5\nTODAY   %s\nLOSSES  %d",trade.count,r,trade.losses);
  char history[240]="";
  for(int i=0;i<trade.count&&i<5;i++){
    char line[48];tm t{};time_t stamp=trade.history_time[i];setenv("TZ","CST-8",1);tzset();localtime_r(&stamp,&t);
    if(trade.history_side[i]=='L'||trade.history_side[i]=='S')
      snprintf(line,sizeof(line),"%02d:%02d  %-5s  %+.1fR\n",t.tm_hour,t.tm_min,trade.history_side[i]=='L'?"LONG":"SHORT",trade.history_half_r[i]/2.0f);
    else snprintf(line,sizeof(line),"--:--  PREVIOUS TRADE\n");
    strncat(history,line,sizeof(history)-strlen(history)-1);
  }
  if(trade.count==0)strcpy(history,"NO TRADES RECORDED");
  lv_label_set_text(journal_history,history);
  if(trade.open){
    uint32_t held=now-trade.entry;lv_label_set_text_fmt(position_label,"%s OPEN  %02d:%02d",trade.side=='L'?"LONG":"SHORT",held/60,held%60);
    lv_label_set_text(journal_status,"SELECT RESULT TO CLOSE");
  }else{
    lv_label_set_text(position_label,"NO OPEN TRADE");
    if(trade.count>=5)lv_label_set_text(journal_status,"DAILY TRADE LIMIT");
    else if(trade.half_r<=-4)lv_label_set_text(journal_status,"DAILY LOSS LIMIT  -2R");
    else if(mes_day_schedule(ny).type==MES_DAY_CLOSED)lv_label_set_text(journal_status,"MARKET CLOSED");
    else if(ny_minutes<570)lv_label_set_text(journal_status,"WAIT FOR 09:30 ET");
    else if(mes_day_schedule(ny).type==MES_DAY_EARLY_CLOSE&&ny_minutes>=mes_day_schedule(ny).close_minutes_et)lv_label_set_text(journal_status,"HOLIDAY SESSION CLOSED");
    else if(ny_minutes>=840&&!after_hours_override)lv_label_set_text(journal_status,"SESSION LOCKED  14:00 ET");
    else if(now<trade.cooldown)lv_label_set_text_fmt(journal_status,"COOLDOWN  %02d:%02d",(trade.cooldown-now)/60,(trade.cooldown-now)%60);
    else if(after_hours_override)lv_label_set_text(journal_status,"ONE TRADE OVERRIDE READY");
    else if(ny_minutes>=780)lv_label_set_text(journal_status,"WRAP UP BEFORE 14:00");
    else lv_label_set_text(journal_status,"READY");
  }
  MesDaySchedule journal_schedule=mes_day_schedule(ny);
  bool trade_limit_lock=!trade.open&&trade.count>=5,loss_limit_lock=!trade.open&&trade.half_r<=-4;
  bool cutoff_lock=journal_schedule.type==MES_DAY_REGULAR&&ny_minutes>=840&&!after_hours_override&&!trade.open&&!trade_limit_lock&&!loss_limit_lock;
  if(session_lock_panel){
    if(trade_limit_lock||loss_limit_lock||cutoff_lock){
      lv_obj_clear_flag(session_lock_panel,LV_OBJ_FLAG_HIDDEN);
      if(loss_limit_lock){
        lv_label_set_text(session_lock_title,"DAILY STOP");lv_label_set_text(session_lock_reason,"-2R LIMIT REACHED\nNo more trades today.");lv_obj_add_flag(override_button,LV_OBJ_FLAG_HIDDEN);
      }else if(trade_limit_lock){
        lv_label_set_text(session_lock_title,"TRADE LIMIT");lv_label_set_text(session_lock_reason,"5 TRADES COMPLETED\nSession discipline complete.");lv_obj_add_flag(override_button,LV_OBJ_FLAG_HIDDEN);
      }else{
        lv_label_set_text(session_lock_title,"SESSION LOCKED");lv_label_set_text(session_lock_reason,"14:00 ET  |  NORMAL TRADING IS OVER\nExceptional setup only: unlocks one trade.");lv_obj_clear_flag(override_button,LV_OBJ_FLAG_HIDDEN);
      }
    }else lv_obj_add_flag(session_lock_panel,LV_OBJ_FLAG_HIDDEN);
  }
  bool entry_locked=trade.open||trading_locked(now);
  if(entry_locked){lv_obj_add_state(long_button,LV_STATE_DISABLED);lv_obj_add_state(short_button,LV_STATE_DISABLED);}
  else{lv_obj_clear_state(long_button,LV_STATE_DISABLED);lv_obj_clear_state(short_button,LV_STATE_DISABLED);}
  if(trade.open)lv_obj_clear_state(cancel_button,LV_STATE_DISABLED);else lv_obj_add_state(cancel_button,LV_STATE_DISABLED);
  for(int i=0;i<7;i++){if(trade.open)lv_obj_clear_state(result_buttons[i],LV_STATE_DISABLED);else lv_obj_add_state(result_buttons[i],LV_STATE_DISABLED);}
}
const char *session_text(const tm &n){
  MesDaySchedule schedule=mes_day_schedule(n);if(schedule.type==MES_DAY_CLOSED)return "MARKET CLOSED";
  int m=n.tm_hour*60+n.tm_min;
  if(schedule.type==MES_DAY_EARLY_CLOSE)return m<570?"HOLIDAY HOURS":(m<schedule.close_minutes_et?"HOLIDAY SESSION":"EARLY CLOSE");
  if(m<570)return "WAIT FOR OPEN";if(m<780)return "TRADING WINDOW";
  if(m<840)return "WRAP-UP WINDOW";return "SESSION LOCKED";
}
void style(lv_obj_t *o,const lv_font_t *f,uint32_t c){lv_obj_set_style_text_font(o,f,LV_PART_MAIN);lv_obj_set_style_text_color(o,lv_color_hex(c),LV_PART_MAIN);}
void discipline_status(const char *text,uint32_t color,bool strong=false){
  lv_label_set_text(discipline_label,text);lv_obj_set_style_text_color(discipline_label,lv_color_hex(color),LV_PART_MAIN);
  lv_obj_set_style_bg_color(discipline_label,lv_color_hex(strong?0x4A171C:BG),LV_PART_MAIN);
  lv_obj_set_style_bg_opa(discipline_label,strong?LV_OPA_COVER:LV_OPA_TRANSP,LV_PART_MAIN);
}
lv_obj_t *card(int x,int y,int w,int h){
  lv_obj_t *o=lv_obj_create(lv_scr_act());lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_bg_color(o,lv_color_hex(PANEL),LV_PART_MAIN);lv_obj_set_style_border_width(o,0,LV_PART_MAIN);
  lv_obj_set_style_radius(o,16,LV_PART_MAIN);lv_obj_set_style_pad_all(o,14,LV_PART_MAIN);
  lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;
}
void ensure_codec(){
  if(codec_ready)return;
  if(codec_init()==ESP_OK){speaker_codec_volume_set(35,nullptr);codec_ready=true;}
}
void short_chime(){
  if(!sound_enabled)return;
  ensure_codec();if(!codec_ready)return;
  constexpr int frames=CODEC_DEFAULT_SAMPLE_RATE/25;
  static int16_t samples[frames*2];
  for(int i=0;i<frames;i++){
    int16_t v=((i/(CODEC_DEFAULT_SAMPLE_RATE/2400))&1)?7000:-7000;
    samples[i*2]=v;samples[i*2+1]=v;
  }
  size_t written=0;speaker_i2s_write(samples,sizeof(samples),&written,pdMS_TO_TICKS(150));
}
void urgent_chime(){
  if(!sound_enabled)return;
  for(int i=0;i<3;i++){short_chime();vTaskDelay(pdMS_TO_TICKS(65));}
}
void toggle_sound(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED)return;
  sound_enabled=!sound_enabled;
  Preferences p;p.begin("mes-settings",false);p.putBool("sound",sound_enabled);p.end();
  lv_label_set_text(sound_label,sound_enabled?"SOUND ON":"SOUND OFF");
  if(sound_enabled)short_chime();
}
void change_interval(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED)return;
  interval_index=(interval_index+1)%(sizeof(intervals)/sizeof(intervals[0]));
  lv_label_set_text_fmt(interval_label,"%dm",intervals[interval_index]);lv_label_set_text(bar_number_label,"--");lv_label_set_text(bar_closed_label,"");
}
void update_clock(lv_timer_t *){
  datetime_t n{};time_t epoch=current_epoch(&n);
  int total=intervals[interval_index]*60,elapsed=n.min*60+n.sec;
  int left=total-(elapsed%total);if(left<=0)left=total;
  lv_label_set_text_fmt(countdown_label,"%02d:%02d",left/60,left%60);
  lv_label_set_text_fmt(beijing_label,"%02d:%02d:%02d",n.hour,n.min,n.sec);
  uint32_t c=left<=10?DANGER:(left<=30?WARN:TEXT);
  lv_obj_set_style_text_color(countdown_label,lv_color_hex(c),LV_PART_MAIN);
  lv_obj_set_style_bg_color(progress_bar,lv_color_hex(c),LV_PART_INDICATOR);
  lv_bar_set_range(progress_bar,0,total);lv_bar_set_value(progress_bar,total-left,LV_ANIM_OFF);
  setenv("TZ","EST5EDT,M3.2.0/2,M11.1.0/2",1);tzset();
  tm ny{};localtime_r(&epoch,&ny);
  lv_label_set_text_fmt(new_york_label,"%02d:%02d:%02d",ny.tm_hour,ny.tm_min,ny.tm_sec);
  lv_label_set_text_fmt(new_york_zone_label,"NEW YORK  |  %s",ny.tm_isdst>0?"EDT":"EST");
  lv_label_set_text(session_label,session_text(ny));
  int ny_minutes=ny.tm_hour*60+ny.tm_min,ny_day=(ny.tm_year+1900)*10000+(ny.tm_mon+1)*100+ny.tm_mday;
  MesDaySchedule schedule=mes_day_schedule(ny);
  int display_close=schedule.type==MES_DAY_EARLY_CLOSE?schedule.close_minutes_et:960;
  if(schedule.type==MES_DAY_CLOSED||ny_minutes<570){
    lv_label_set_text_fmt(interval_label,"%dm",intervals[interval_index]);lv_label_set_text(bar_number_label,"--");lv_label_set_text(bar_closed_label,"");
  }else if(ny_minutes<display_close){
    int seconds_from_open=(ny_minutes-570)*60+ny.tm_sec;
    int bar_number=seconds_from_open/(intervals[interval_index]*60)+1;
    lv_label_set_text_fmt(interval_label,"%dm",intervals[interval_index]);lv_label_set_text_fmt(bar_number_label,"%d",bar_number);lv_label_set_text(bar_closed_label,"");
  }else{
    int final_bar=(display_close-570)/intervals[interval_index];
    lv_label_set_text_fmt(interval_label,"%dm",intervals[interval_index]);lv_label_set_text_fmt(bar_number_label,"%d",final_bar);lv_label_set_text(bar_closed_label,"CLOSED");
  }
  if(trade.half_r<=-4){
    discipline_status("DAILY STOP  |  -2R LIMIT REACHED",DANGER,true);
  }else if(trade.count>=5){
    discipline_status("TRADE LIMIT  |  5 / 5 COMPLETE",DANGER,true);
  }else if(schedule.type==MES_DAY_CLOSED){
    char status[96];snprintf(status,sizeof(status),"MARKET CLOSED  |  %s",schedule.name);discipline_status(status,MUTED);
  }else if(ny_minutes<570){
    int to_open=570*60-(ny.tm_hour*3600+ny.tm_min*60+ny.tm_sec);
    char status[64];snprintf(status,sizeof(status),"OPEN IN  %02d:%02d:%02d",to_open/3600,(to_open%3600)/60,to_open%60);discipline_status(status,ACCENT);
  }else if(schedule.type==MES_DAY_EARLY_CLOSE&&ny_minutes<schedule.close_minutes_et){
    char status[112];snprintf(status,sizeof(status),"HOLIDAY HOURS  |  %s  |  13:00 LOCK",schedule.name);discipline_status(status,WARN);
  }else if(schedule.type==MES_DAY_EARLY_CLOSE){
    char status[96];snprintf(status,sizeof(status),"EARLY CLOSE  |  %s",schedule.name);discipline_status(status,DANGER,true);
    if(last_1300_alert_day!=ny_day){last_1300_alert_day=ny_day;urgent_chime();}
  }else if(ny_minutes<780){
    discipline_status("TRADING WINDOW  |  13:00 LIMIT",0x219653);
  }else if(ny_minutes<840){
    discipline_status("13:00 LIMIT PASSED  |  WRAP UP BY 14:00",DANGER,true);
    if(last_1300_alert_day!=ny_day){last_1300_alert_day=ny_day;urgent_chime();}
  }else{
    discipline_status("SESSION LOCKED  |  14:00 ET",DANGER,true);
  }
  if(sound_enabled&&n.sec==0&&(n.min%intervals[interval_index])==0&&epoch!=last_chime_epoch){
    last_chime_epoch=epoch;short_chime();
  }
  if(network_status_label){
    if(test_mode)
      lv_label_set_text_fmt(network_status_label,"TEST MODE  |  SERIAL: TEST,OFF  |  %s",FIRMWARE_VERSION);
    else if(network_hold_started){
      uint32_t held=millis()-network_hold_started;int left=held>=5000?0:(int)((5000-held+999)/1000);
      lv_label_set_text_fmt(network_status_label,"%s  |  KEEP HOLDING  %d",time_check_required?"CONFIRM TIME":"WIFI SETUP",left);
    }else if(network_state==NET_ONLINE&&WiFi.status()==WL_CONNECTED&&time_check_required)
      lv_label_set_text_fmt(network_status_label,"TIME CHECK  |  HOLD 5S TO SYNC  |  %s",FIRMWARE_VERSION);
    else if(network_state==NET_ONLINE&&WiFi.status()==WL_CONNECTED)
      lv_label_set_text_fmt(network_status_label,"ONLINE  |  %s  |  %s",time_synced?"SYNCED":"SYNCING",FIRMWARE_VERSION);
    else if(portal_active)
      lv_label_set_text(network_status_label,"SETUP  |  192.168.4.1");
    else
      lv_label_set_text_fmt(network_status_label,"CONNECTING  |  RTC  |  %s",FIRMWARE_VERSION);
  }
}
lv_obj_t *action_button(lv_obj_t *parent,const char *text,int x,int y,int w,int h,uint32_t color,lv_event_cb_t cb,void *data=nullptr){
  lv_obj_t *b=lv_btn_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
  lv_obj_set_style_bg_color(b,lv_color_hex(color),LV_PART_MAIN);lv_obj_set_style_radius(b,12,LV_PART_MAIN);
  lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,data);lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,text);style(l,&lv_font_montserrat_16,TEXT);lv_obj_center(l);return b;
}
void show_timer(lv_event_t *){lv_scr_load(timer_screen);}
void show_journal(lv_event_t *){refresh_journal(nullptr);lv_scr_load(journal_screen);}
void open_trade(lv_event_t *e){
  time_t now=current_epoch();if(trade.open||trading_locked(now))return;
  if(new_york_minutes(now)>=840)after_hours_override=false;
  trade.open=true;trade.side=(char)(intptr_t)lv_event_get_user_data(e);trade.entry=now;save_trade();refresh_journal(nullptr);lv_scr_load(timer_screen);
}
void record_result(int result){
  if(!trade.open)return;time_t now=current_epoch();
  if(trade.count<5){trade.history_side[trade.count]=trade.side;trade.history_half_r[trade.count]=result;trade.history_time[trade.count]=now;}
  trade.open=false;trade.count++;trade.half_r+=result;trade.losses=result<0?trade.losses+1:0;
  trade.side='-';trade.entry=0;trade.cooldown=now+(trade.losses>=2?900:300);save_trade();refresh_journal(nullptr);
}
void close_trade(lv_event_t *e){
  record_result((int)(intptr_t)lv_event_get_user_data(e));
}
void cancel_trade(lv_event_t *){
  if(!trade.open)return;trade.open=false;trade.side='-';trade.entry=0;save_trade();refresh_journal(nullptr);
}
void undo_last(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED||trade.open||trade.count==0)return;
  time_t now=current_epoch();
  if(now>undo_armed_until){undo_armed_until=now+3;lv_label_set_text(undo_label,"CONFIRM");return;}
  int i=trade.count-1;trade.half_r-=trade.history_half_r[i];trade.history_side[i]=0;trade.history_half_r[i]=0;trade.history_time[i]=0;
  trade.count--;trade.losses=0;
  for(int j=trade.count-1;j>=0&&trade.history_half_r[j]<0;j--)trade.losses++;
  trade.cooldown=0;undo_armed_until=0;lv_label_set_text(undo_label,"UNDO");save_trade();refresh_journal(nullptr);
}
void override_hold(lv_event_t *e){
  lv_event_code_t code=lv_event_get_code(e);
  if(code==LV_EVENT_PRESSED){override_press_started=lv_tick_get();lv_label_set_text(override_button_label,"KEEP HOLDING  5");}
  else if(code==LV_EVENT_PRESSING&&override_press_started){
    uint32_t elapsed=lv_tick_get()-override_press_started;
    if(elapsed>=5000){
      after_hours_override=true;override_press_started=0;lv_label_set_text(override_button_label,"ONE TRADE UNLOCKED");
      lv_obj_add_flag(session_lock_panel,LV_OBJ_FLAG_HIDDEN);refresh_journal(nullptr);
    }else lv_label_set_text_fmt(override_button_label,"KEEP HOLDING  %d",(5000-elapsed+999)/1000);
  }else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){
    override_press_started=0;if(!after_hours_override)lv_label_set_text(override_button_label,"HOLD 5s TO UNLOCK ONE TRADE");
  }
}
void hide_custom(lv_event_t *){lv_obj_add_flag(custom_panel,LV_OBJ_FLAG_HIDDEN);}
void adjust_custom(lv_event_t *e){
  int step=(int)(intptr_t)lv_event_get_user_data(e);custom_half_r+=step;
  if(custom_half_r>20)custom_half_r=20;if(custom_half_r<-4)custom_half_r=-4;
  lv_label_set_text_fmt(custom_value_label,"%+.1fR",custom_half_r/2.0f);
}
void show_custom(lv_event_t *){
  if(!trade.open)return;
  custom_half_r=5;lv_label_set_text(custom_value_label,"+2.5R");
  lv_obj_clear_flag(custom_panel,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(custom_panel);
}
void confirm_custom(lv_event_t *){
  if(!trade.open){lv_obj_add_flag(custom_panel,LV_OBJ_FLAG_HIDDEN);return;}
  record_result(custom_half_r);
  lv_obj_add_flag(custom_panel,LV_OBJ_FLAG_HIDDEN);
}
void reset_today(lv_event_t *e){
  if(lv_event_get_code(e)!=LV_EVENT_CLICKED)return;
  time_t now=current_epoch();
  if(now>reset_armed_until){
    reset_armed_until=now+3;lv_label_set_text(reset_label,"CONFIRM");return;
  }
  time_t epoch=current_epoch();trade=TradeState{};trade.day=new_york_day_key(epoch);after_hours_override=false;
  reset_armed_until=0;save_trade();lv_label_set_text(reset_label,"RESET");refresh_journal(nullptr);
}
void create_journal_ui(){
  journal_screen=lv_obj_create(nullptr);lv_obj_set_style_bg_color(journal_screen,lv_color_hex(BG),LV_PART_MAIN);
  lv_obj_t *title=lv_label_create(journal_screen);lv_label_set_text(title,"TODAY'S TRADES");style(title,&lv_font_montserrat_26,TEXT);lv_obj_set_pos(title,28,18);
  lv_obj_t *undo=action_button(journal_screen,"UNDO",380,12,116,48,WARN,undo_last);undo_label=lv_obj_get_child(undo,0);
  action_button(journal_screen,"BACK",660,12,116,48,PANEL,show_timer);
  lv_obj_t *reset=action_button(journal_screen,"RESET",520,12,116,48,DANGER,reset_today);
  reset_label=lv_obj_get_child(reset,0);
  lv_obj_t *summary=lv_obj_create(journal_screen);lv_obj_set_pos(summary,24,70);lv_obj_set_size(summary,220,305);
  lv_obj_set_style_bg_color(summary,lv_color_hex(PANEL),LV_PART_MAIN);lv_obj_set_style_border_width(summary,0,LV_PART_MAIN);lv_obj_set_style_radius(summary,16,LV_PART_MAIN);
  lv_obj_clear_flag(summary,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scrollbar_mode(summary,LV_SCROLLBAR_MODE_OFF);
  journal_summary=lv_label_create(summary);style(journal_summary,&lv_font_montserrat_16,TEXT);lv_obj_align(journal_summary,LV_ALIGN_TOP_LEFT,5,5);
  journal_history=lv_label_create(summary);style(journal_history,&lv_font_montserrat_16,MUTED);lv_obj_set_pos(journal_history,5,72);
  journal_status=lv_label_create(summary);style(journal_status,&lv_font_montserrat_16,WARN);lv_obj_align(journal_status,LV_ALIGN_BOTTOM_LEFT,5,-8);
  position_label=lv_label_create(journal_screen);lv_label_set_text(position_label,"NO OPEN TRADE");style(position_label,&lv_font_montserrat_26,ACCENT);lv_obj_set_pos(position_label,285,78);
  long_button=action_button(journal_screen,"LONG",280,125,220,70,0x219653,open_trade,(void *)(intptr_t)'L');
  short_button=action_button(journal_screen,"SHORT",530,125,220,70,DANGER,open_trade,(void *)(intptr_t)'S');
  lv_obj_t *result=lv_label_create(journal_screen);lv_label_set_text(result,"CLOSE RESULT");style(result,&lv_font_montserrat_16,MUTED);lv_obj_set_pos(result,280,220);
  cancel_button=action_button(journal_screen,"CANCEL OPEN",610,207,140,40,MUTED,cancel_trade);
  const char *names[]={"-1R","-0.5R","0R","+0.5R","+1R","+2R"};const int values[]={-2,-1,0,1,2,4};
  const int result_x[]={280,401,522,644};
  for(int i=0;i<6;i++){uint32_t c=i<2?DANGER:(i==2?MUTED:0x219653);result_buttons[i]=action_button(journal_screen,names[i],result_x[i%4],255+(i/4)*66,106,52,c,close_trade,(void *)(intptr_t)values[i]);}
  result_buttons[6]=action_button(journal_screen,"CUSTOM",522,321,228,52,ACCENT,show_custom);
  lv_obj_t *rule=lv_label_create(journal_screen);lv_label_set_text(rule,"RULES: 5 TRADES  |  -2R STOP  |  5m WAIT  |  2 LOSSES = 15m");style(rule,&lv_font_montserrat_16,MUTED);lv_obj_align(rule,LV_ALIGN_BOTTOM_MID,0,-22);

  custom_panel=lv_obj_create(journal_screen);lv_obj_set_size(custom_panel,470,250);lv_obj_center(custom_panel);
  lv_obj_set_style_bg_color(custom_panel,lv_color_hex(0x202B3B),LV_PART_MAIN);lv_obj_set_style_border_color(custom_panel,lv_color_hex(ACCENT),LV_PART_MAIN);
  lv_obj_set_style_border_width(custom_panel,2,LV_PART_MAIN);lv_obj_set_style_radius(custom_panel,18,LV_PART_MAIN);lv_obj_clear_flag(custom_panel,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *custom_title=lv_label_create(custom_panel);lv_label_set_text(custom_title,"CUSTOM RESULT");style(custom_title,&lv_font_montserrat_16,TEXT);lv_obj_align(custom_title,LV_ALIGN_TOP_MID,0,4);
  custom_value_label=lv_label_create(custom_panel);lv_label_set_text(custom_value_label,"+2.5R");style(custom_value_label,&lv_font_montserrat_44,TEXT);lv_obj_align(custom_value_label,LV_ALIGN_CENTER,0,-20);
  action_button(custom_panel,"-0.5R",28,78,120,58,DANGER,adjust_custom,(void *)(intptr_t)-1);
  action_button(custom_panel,"+0.5R",290,78,120,58,0x219653,adjust_custom,(void *)(intptr_t)1);
  action_button(custom_panel,"CANCEL",28,165,170,52,MUTED,hide_custom);
  action_button(custom_panel,"CONFIRM",240,165,170,52,ACCENT,confirm_custom);
  lv_obj_add_flag(custom_panel,LV_OBJ_FLAG_HIDDEN);

  session_lock_panel=lv_obj_create(journal_screen);lv_obj_set_size(session_lock_panel,560,300);lv_obj_center(session_lock_panel);
  lv_obj_set_style_bg_color(session_lock_panel,lv_color_hex(0x241519),LV_PART_MAIN);lv_obj_set_style_border_color(session_lock_panel,lv_color_hex(DANGER),LV_PART_MAIN);
  lv_obj_set_style_border_width(session_lock_panel,3,LV_PART_MAIN);lv_obj_set_style_radius(session_lock_panel,18,LV_PART_MAIN);lv_obj_clear_flag(session_lock_panel,LV_OBJ_FLAG_SCROLLABLE);
  session_lock_title=lv_label_create(session_lock_panel);lv_label_set_text(session_lock_title,"SESSION LOCKED");style(session_lock_title,&lv_font_montserrat_44,DANGER);lv_obj_align(session_lock_title,LV_ALIGN_TOP_MID,0,18);
  session_lock_reason=lv_label_create(session_lock_panel);lv_label_set_text(session_lock_reason,"14:00 ET  |  NORMAL TRADING IS OVER\nExceptional setup only: unlocks one trade.");
  style(session_lock_reason,&lv_font_montserrat_16,TEXT);lv_obj_set_style_text_align(session_lock_reason,LV_TEXT_ALIGN_CENTER,LV_PART_MAIN);lv_obj_align(session_lock_reason,LV_ALIGN_CENTER,0,-18);
  override_button=lv_btn_create(session_lock_panel);lv_obj_set_size(override_button,420,64);lv_obj_align(override_button,LV_ALIGN_BOTTOM_MID,0,-20);
  lv_obj_set_style_bg_color(override_button,lv_color_hex(DANGER),LV_PART_MAIN);lv_obj_set_style_radius(override_button,12,LV_PART_MAIN);
  lv_obj_add_event_cb(override_button,override_hold,LV_EVENT_ALL,nullptr);
  override_button_label=lv_label_create(override_button);lv_label_set_text(override_button_label,"HOLD 5s TO UNLOCK ONE TRADE");style(override_button_label,&lv_font_montserrat_16,TEXT);lv_obj_center(override_button_label);
  lv_obj_add_flag(session_lock_panel,LV_OBJ_FLAG_HIDDEN);
  refresh_journal(nullptr);lv_timer_create(refresh_journal,1000,nullptr);
}
void create_ui(){
  timer_screen=lv_scr_act();lv_obj_set_style_bg_color(timer_screen,lv_color_hex(BG),LV_PART_MAIN);
  lv_obj_t *title=lv_label_create(lv_scr_act());lv_label_set_text(title,"TRADING SESSION");style(title,&lv_font_montserrat_26,MUTED);lv_obj_set_pos(title,28,20);
  session_label=lv_label_create(lv_scr_act());lv_label_set_text(session_label,"INITIALISING");style(session_label,&lv_font_montserrat_16,ACCENT);lv_obj_align(session_label,LV_ALIGN_TOP_RIGHT,-28,20);

  lv_obj_t *tc=card(24,62,500,298);
  lv_obj_t *bar_row=lv_obj_create(tc);lv_obj_set_pos(bar_row,4,0);lv_obj_set_size(bar_row,310,36);
  lv_obj_set_style_bg_opa(bar_row,LV_OPA_TRANSP,LV_PART_MAIN);lv_obj_set_style_border_width(bar_row,0,LV_PART_MAIN);lv_obj_set_style_pad_all(bar_row,0,LV_PART_MAIN);
  lv_obj_set_style_pad_column(bar_row,8,LV_PART_MAIN);lv_obj_clear_flag(bar_row,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar_row,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(bar_row,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  interval_label=lv_label_create(bar_row);lv_label_set_text(interval_label,"5m");style(interval_label,&lv_font_montserrat_26,ACCENT);
  lv_obj_t *bar_caption=lv_label_create(bar_row);lv_label_set_text(bar_caption,"|  BAR");style(bar_caption,&lv_font_montserrat_18,ACCENT);
  bar_number_label=lv_label_create(bar_row);lv_label_set_text(bar_number_label,"--");style(bar_number_label,&lv_font_montserrat_26,ACCENT);
  bar_closed_label=lv_label_create(bar_row);lv_label_set_text(bar_closed_label,"");style(bar_closed_label,&lv_font_montserrat_16,MUTED);
  lv_obj_t *button=lv_btn_create(tc);lv_obj_set_size(button,120,58);lv_obj_align(button,LV_ALIGN_TOP_RIGHT,0,-4);
  lv_obj_set_style_bg_color(button,lv_color_hex(ACCENT),LV_PART_MAIN);lv_obj_set_style_radius(button,12,LV_PART_MAIN);
  lv_obj_add_event_cb(button,change_interval,LV_EVENT_CLICKED,nullptr);
  lv_obj_t *bl=lv_label_create(button);lv_label_set_text(bl,"CHANGE");style(bl,&lv_font_montserrat_16,TEXT);lv_obj_center(bl);
  countdown_label=lv_label_create(tc);lv_label_set_text(countdown_label,"05:00");style(countdown_label,&lv_font_montserrat_44,TEXT);lv_obj_align(countdown_label,LV_ALIGN_CENTER,0,-8);
  progress_bar=lv_bar_create(tc);lv_obj_set_size(progress_bar,438,14);lv_obj_align(progress_bar,LV_ALIGN_BOTTOM_MID,0,-12);
  lv_obj_set_style_bg_color(progress_bar,lv_color_hex(0x273345),LV_PART_MAIN);lv_obj_set_style_bg_color(progress_bar,lv_color_hex(ACCENT),LV_PART_INDICATOR);
  lv_obj_set_style_radius(progress_bar,7,LV_PART_MAIN);lv_obj_set_style_radius(progress_bar,7,LV_PART_INDICATOR);

  lv_obj_t *cc=card(544,62,232,298);
  lv_obj_t *market_section=lv_obj_create(cc);lv_obj_set_pos(market_section,0,0);lv_obj_set_size(market_section,202,134);
  lv_obj_set_style_bg_opa(market_section,LV_OPA_TRANSP,LV_PART_MAIN);lv_obj_set_style_border_width(market_section,0,LV_PART_MAIN);
  lv_obj_set_style_pad_all(market_section,0,LV_PART_MAIN);lv_obj_clear_flag(market_section,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *market_group=lv_obj_create(market_section);lv_obj_set_size(market_group,202,88);lv_obj_align(market_group,LV_ALIGN_CENTER,0,3);
  lv_obj_set_style_bg_opa(market_group,LV_OPA_TRANSP,LV_PART_MAIN);lv_obj_set_style_border_width(market_group,0,LV_PART_MAIN);
  lv_obj_set_style_pad_all(market_group,0,LV_PART_MAIN);lv_obj_clear_flag(market_group,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(market_group,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(market_group,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER);
  lv_obj_t *ct=lv_label_create(market_group);lv_label_set_text(ct,"MARKET CLOCK");style(ct,&lv_font_montserrat_16,MUTED);
  new_york_zone_label=lv_label_create(market_group);lv_label_set_text(new_york_zone_label,"NEW YORK  |  EST");style(new_york_zone_label,&lv_font_montserrat_16,MUTED);
  new_york_label=lv_label_create(market_group);lv_label_set_text(new_york_label,"--:--:--");style(new_york_label,&lv_font_montserrat_26,TEXT);
  lv_obj_t *divider=lv_obj_create(cc);lv_obj_set_pos(divider,0,135);lv_obj_set_size(divider,202,1);
  lv_obj_set_style_bg_color(divider,lv_color_hex(0x334155),LV_PART_MAIN);lv_obj_set_style_border_width(divider,0,LV_PART_MAIN);lv_obj_set_style_pad_all(divider,0,LV_PART_MAIN);lv_obj_clear_flag(divider,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *local_section=lv_obj_create(cc);lv_obj_set_pos(local_section,0,136);lv_obj_set_size(local_section,202,134);
  lv_obj_set_style_bg_opa(local_section,LV_OPA_TRANSP,LV_PART_MAIN);lv_obj_set_style_border_width(local_section,0,LV_PART_MAIN);
  lv_obj_set_style_pad_all(local_section,0,LV_PART_MAIN);lv_obj_clear_flag(local_section,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *local_group=lv_obj_create(local_section);lv_obj_set_size(local_group,202,88);lv_obj_align(local_group,LV_ALIGN_CENTER,0,3);
  lv_obj_set_style_bg_opa(local_group,LV_OPA_TRANSP,LV_PART_MAIN);lv_obj_set_style_border_width(local_group,0,LV_PART_MAIN);
  lv_obj_set_style_pad_all(local_group,0,LV_PART_MAIN);lv_obj_clear_flag(local_group,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(local_group,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(local_group,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER);
  lv_obj_t *local_title=lv_label_create(local_group);lv_label_set_text(local_title,"LOCAL TIME");style(local_title,&lv_font_montserrat_16,MUTED);
  lv_obj_t *local_zone=lv_label_create(local_group);lv_label_set_text(local_zone,"BEIJING  |  UTC+8");style(local_zone,&lv_font_montserrat_16,MUTED);
  beijing_label=lv_label_create(local_group);lv_label_set_text(beijing_label,"--:--:--");style(beijing_label,&lv_font_montserrat_26,TEXT);
  discipline_label=lv_label_create(timer_screen);lv_label_set_text(discipline_label,"CHECKING SESSION");style(discipline_label,&lv_font_montserrat_16,MUTED);
  lv_obj_set_size(discipline_label,752,34);lv_obj_set_style_text_align(discipline_label,LV_TEXT_ALIGN_CENTER,LV_PART_MAIN);
  lv_obj_set_style_radius(discipline_label,8,LV_PART_MAIN);lv_obj_set_style_pad_top(discipline_label,7,LV_PART_MAIN);lv_obj_set_pos(discipline_label,24,374);
  home_trades_button=action_button(timer_screen,"TRADES",24,418,128,44,ACCENT,show_journal);
  home_trades_label=lv_obj_get_child(home_trades_button,0);
  network_status_label=lv_label_create(timer_screen);lv_label_set_text_fmt(network_status_label,"WIFI CONNECTING  |  RTC ACTIVE  |  %s",FIRMWARE_VERSION);
  style(network_status_label,&lv_font_montserrat_16,MUTED);lv_obj_align(network_status_label,LV_ALIGN_BOTTOM_MID,0,-30);
  lv_obj_add_flag(network_status_label,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(network_status_label,network_setup_hold,LV_EVENT_ALL,nullptr);
  lv_obj_t *sound=lv_btn_create(lv_scr_act());lv_obj_set_size(sound,128,44);lv_obj_align(sound,LV_ALIGN_BOTTOM_RIGHT,-24,-18);
  lv_obj_set_style_bg_color(sound,lv_color_hex(PANEL),LV_PART_MAIN);lv_obj_set_style_radius(sound,10,LV_PART_MAIN);
  lv_obj_add_event_cb(sound,toggle_sound,LV_EVENT_CLICKED,nullptr);
  sound_label=lv_label_create(sound);lv_label_set_text(sound_label,sound_enabled?"SOUND ON":"SOUND OFF");style(sound_label,&lv_font_montserrat_16,sound_enabled?ACCENT:MUTED);lv_obj_center(sound_label);
  update_clock(nullptr);lv_timer_create(update_clock,1000,nullptr);
  create_journal_ui();
}
}
void setup(){
  Serial.begin(115200);DEV_I2C_Init();IO_EXTENSION_Init();PCF85063A_Init();initialise_rtc_once();
  Preferences settings;settings.begin("mes-settings",true);sound_enabled=settings.getBool("sound",false);settings.end();
  load_trade();
  esp_lcd_touch_handle_t touch=touch_gt911_init(DEV_I2C_Get_Bus_Device());
  esp_lcd_panel_handle_t panel=waveshare_esp32_s3_rgb_lcd_init();waveshare_rgb_lcd_bl_on();
  ESP_ERROR_CHECK(lvgl_port_init(panel,touch));if(lvgl_port_lock(-1)){create_ui();lvgl_port_unlock();}
  begin_wifi();
  Serial.println("MES_READY");
}
void loop(){
  service_wifi();
  if(Serial.available()){
    String line=Serial.readStringUntil('\n');line.trim();
    int y,mo,d,h,mi,s;
    if(sscanf(line.c_str(),"TEST,NY,%d,%d,%d,%d,%d,%d",&y,&mo,&d,&h,&mi,&s)==6){
      tm simulated{};simulated.tm_year=y-1900;simulated.tm_mon=mo-1;simulated.tm_mday=d;
      simulated.tm_hour=h;simulated.tm_min=mi;simulated.tm_sec=s;simulated.tm_isdst=-1;
      setenv("TZ","EST5EDT,M3.2.0/2,M11.1.0/2",1);tzset();test_epoch_base=mktime(&simulated);test_millis_base=millis();test_mode=true;
      trade=TradeState{};trade.day=new_york_day_key(test_epoch_base);after_hours_override=false;
      MesDaySchedule schedule=mes_day_schedule(simulated);
      int test_minutes=h*60+mi;const char *phase;
      if(schedule.type==MES_DAY_CLOSED)phase="CLOSED";
      else if(test_minutes<570)phase="PREOPEN";
      else if(schedule.type==MES_DAY_EARLY_CLOSE)phase=test_minutes<schedule.close_minutes_et?"HOLIDAY":"EARLY_CLOSED";
      else if(test_minutes<780)phase="TRADING";
      else if(test_minutes<840)phase="WRAP_UP";
      else phase="LOCKED";
      Serial.printf("TEST_OK NY=%04d-%02d-%02d %02d:%02d:%02d TYPE=%d CLOSE=%d PHASE=%s ENTRY=%s DST=%d NAME=%s\n",
        y,mo,d,h,mi,s,(int)schedule.type,schedule.close_minutes_et,phase,session_allows_entry(test_epoch_base)?"YES":"NO",simulated.tm_isdst,schedule.name);
    }else if(line=="TEST,OFF"){
      test_mode=false;after_hours_override=false;load_trade();Serial.println("TEST_OFF");
    }else if(sscanf(line.c_str(),"TIME,%d,%d,%d,%d,%d,%d",&y,&mo,&d,&h,&mi,&s)==6){
      datetime_t t{(UWORD)y,(UBYTE)mo,(UBYTE)d,(UBYTE)weekday(y,mo,d),(UBYTE)h,(UBYTE)mi,(UBYTE)s};
      PCF85063A_Set_All(t);
      Serial.println("TIME_OK");
    }else Serial.println("COMMAND_ERROR");
  }
  delay(20);
}
