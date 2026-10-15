#pragma once
// Krunker v10.0.2's Svelte popups the C port never built (client/src/ui/v10_misc_popups.c):
//   survey (svelte-1u0qxrf)         a notification item in #notificationCenter (the chat holder), pushed by GAME_SURVEY
//   mission briefing (1849bws)      the guest "Operation: Ghost Recruit" retention modal
//   k-ID dialog (lbi007)            GDC 2025 display mode intro, fly y:20 over 500 ms
//   lottery / raffle (1rj22es)      openLottery(id): defaultModal lotteryModal; eggs (gt9grt), gifts
//                                   (wjqbtw) and light bulbs (1fenhdo) decorate its border
//   premium subscription shop (190e6fj)   the Store Premium tab's subscription branch
//   usernames (27udiy)              manageAccNames(): the Account window's "Manage" popup
// What v10.0.2 itself never reaches (its premium branch is behind `() => !0x0`, the mission briefing's
// manager is never enabled) is built and reachable only through `knMiscOpen("<name>")` and the
// --render-scenario state.community.popups; see the file's header for each popup's backend.
#include <cJSON.h>
#include <html_dom.h>

void v10_misc_popups_setup(void);                 // the page is up (called from v10_news_setup)
void v10_misc_popups_tick(void);                  // per frame: countdowns, expiry
int  v10_misc_popups_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 not ours
// --render-scenario state.community.popups: {open, survey, hook, kid, lottery, lotterySecondsLeft, premium, usernames}
void v10_misc_popups_scenario(const cJSON *popups);
void v10_misc_survey_show(const cJSON *survey);   // GAME_SURVEY: {surveyId, question, answers[], expiresAt (unix s), rewardAmount}
void v10_misc_survey_clear(void);                 // surveyId "CLEAR_SURVEY"
void v10_misc_guest_progress(double kr, double score); // the guest's earnings the mission briefing reports
