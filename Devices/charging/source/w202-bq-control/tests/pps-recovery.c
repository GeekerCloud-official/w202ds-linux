/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute the production PPS client against a deterministic power_supply ABI.
 * Busy exits send notifications too; lock and reference assertions catch
 * deadlocks/leaks independently of voltage-selection boundary tests.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define W202_BQ_CHARGER_H
#define min(a,b) ((a) < (b) ? (a) : (b))
#define READ_ONCE(a) (a)
#define POWER_SUPPLY_HEALTH_GOOD 1
#define POWER_SUPPLY_USB_TYPE_PD_PPS 8
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
typedef int atomic_t;
static int atomic_xchg(atomic_t *a,int value) { int old=*a; *a=value; return old; }
struct mutex { int locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked=1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked=0; }
struct i2c_client { int dev; };
struct w202_bq {
    struct i2c_client *client;
    struct mutex lock;
    bool slave, stop_requested, suspended, shutting_down, thermal_blocked, fault_latched;
    bool auto_pps, pps_blocked, pps_exit_pending;
    int health, max_input_uv;
    const char *pps_supply;
    unsigned long pps_next_refresh;
    unsigned int pps_retries;
    atomic_t cable_detached;
};
struct power_supply { int unused; };
union power_supply_propval { int intval; };
enum power_supply_property { POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_USB_TYPE,
    POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_MAX,
    POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_VOLTAGE_MIN, POWER_SUPPLY_PROP_VOLTAGE_MAX };
unsigned long jiffies;
static struct w202_bq bq;
static struct power_supply source;
static int online, voltage, budget, requested_current, refs, writes, busy_exits;
static bool input_online, available, stop_during_activation;
static struct power_supply *power_supply_get_by_name(const char *name)
{
    (void)name;
    if (!available) return NULL;
    ++refs;
    return &source;
}
static void power_supply_put(struct power_supply *psy) { assert(psy==&source); assert(refs>0); --refs; }
static int power_supply_get_property(struct power_supply *psy, enum power_supply_property property,
                                    union power_supply_propval *val)
{
    assert(psy==&source);
    switch(property) {
    case POWER_SUPPLY_PROP_ONLINE: val->intval=online; break;
    case POWER_SUPPLY_PROP_USB_TYPE: val->intval=POWER_SUPPLY_USB_TYPE_PD_PPS; break;
    case POWER_SUPPLY_PROP_VOLTAGE_NOW: val->intval=voltage; break;
    case POWER_SUPPLY_PROP_CURRENT_MAX: val->intval=budget; break;
    case POWER_SUPPLY_PROP_CURRENT_NOW: val->intval=requested_current; break;
    case POWER_SUPPLY_PROP_VOLTAGE_MIN: val->intval=3300000; break;
    case POWER_SUPPLY_PROP_VOLTAGE_MAX: val->intval=11000000; break;
    }
    return 0;
}
static int power_supply_set_property(struct power_supply *psy, enum power_supply_property property,
                                    const union power_supply_propval *val)
{
    assert(psy==&source && !bq.lock.locked);
    ++writes;
    if (property==POWER_SUPPLY_PROP_ONLINE) {
        if(val->intval==1 && busy_exits) { --busy_exits; return -EAGAIN; }
        online=val->intval;
        if(online==1) voltage=9000000;
        if(online==2 && stop_during_activation) bq.stop_requested=true;
    } else if(property==POWER_SUPPLY_PROP_CURRENT_NOW) {
        assert(online==2 && val->intval<=budget);
        requested_current=val->intval;
    } else if(property==POWER_SUPPLY_PROP_VOLTAGE_NOW) {
        assert(online==2 && val->intval>=5000000 && val->intval<=9800000);
        voltage=val->intval;
    } else assert(0);
    return 0;
}
static int w202_bq_supply_property(const char *name, enum power_supply_property property, int *value)
{
    (void)name; assert(property==POWER_SUPPLY_PROP_ONLINE); *value=input_online; return 0;
}
#include "../pps.c"
static int step(unsigned long elapsed)
{
    int ret;
    jiffies+=elapsed;
    ret=w202_bq_refresh_pps(&bq);
    assert(refs==0 && !bq.lock.locked);
    return ret;
}
static void reset(void)
{
    memset(&bq,0,sizeof(bq));
    bq.auto_pps=true; bq.health=POWER_SUPPLY_HEALTH_GOOD;
    bq.max_input_uv=12000000; bq.pps_supply="test-source";
    online=2; voltage=9800000; requested_current=budget=3000000;
    writes=refs=busy_exits=0; jiffies=10000;
    available=input_online=true; stop_during_activation=false;
}
int main(void)
{
    int i, previous;
    reset();
    bq.health=0; busy_exits=1;
    assert(step(0)==-EAGAIN && bq.pps_exit_pending && writes==1);
    previous=writes;
    for(i=0;i<100;i++) assert(step(0)==-EAGAIN);
    assert(writes==previous && !bq.stop_requested);
    assert(step(2000)==0 && online==1 && !bq.pps_exit_pending);
    bq.health=POWER_SUPPLY_HEALTH_GOOD;
    for(i=0;i<15;i++) assert(step(2000)==0);
    assert(online==2 && voltage==9800000 && !bq.stop_requested);
    puts("PASS: busy exit is paced, auto survives, fixed contract then PPS recover");

    reset(); input_online=false; busy_exits=2;
    assert(step(0)==-EAGAIN && bq.pps_blocked && bq.pps_exit_pending);
    previous=writes;
    for(i=0;i<100;i++) assert(step(0)==-EAGAIN);
    assert(writes==previous);
    assert(step(2000)==-EAGAIN && bq.pps_exit_pending);
    assert(step(2000)==0 && online==1 && !bq.pps_exit_pending && !bq.stop_requested);
    input_online=true;
    assert(step(4000)==0 && online==1); /* failed attachment stays fixed */
    bq.cable_detached=1; /* quick off/on completed while worker was busy */
    assert(step(0)==0 && online==2 && !bq.pps_blocked);
    puts("PASS: input loss waits for confirmed exit; brief disconnect resets retry latch");

    reset(); online=1; voltage=9000000; stop_during_activation=true;
    assert(step(0)==-EAGAIN && bq.stop_requested && bq.pps_exit_pending);
    assert(step(2000)==0 && online==1);
    for(i=0;i<10;i++) assert(step(2000)==0 && online==1 && bq.stop_requested);
    puts("PASS: manual inhibit during synchronous negotiation remains authoritative");

    reset(); bq.thermal_blocked=true;
    assert(step(0)==0 && online==1 && !bq.stop_requested);
    bq.thermal_blocked=false;
    assert(step(2000)==0 && online==2);
    online=0; bq.pps_blocked=true; bq.pps_exit_pending=true;
    assert(step(0)==0 && !bq.pps_blocked && !bq.pps_exit_pending);
    puts("PASS: protection exit preserves request; offline clears pending state");
    return 0;
}
