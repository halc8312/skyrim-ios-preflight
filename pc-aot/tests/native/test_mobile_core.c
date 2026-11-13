/* Authored host-only inputs. Does not load or execute a game. */
#include "mobile_core.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void *producer(void *arg) {
    (void)arg;
    for(unsigned i=0;i<20000;i++) {
        skyrim_mobile_button(12,i&1);skyrim_mobile_button(SKYRIM_PAD_RT,i&1);
        skyrim_mobile_move((i&1) ? .5f : 0,0,.1f);
        skyrim_mobile_look(10,3,.016f,440,1.5f,0);
        if(!(i%10)) skyrim_mobile_clear();
    }
    return NULL;
}
int main(void) {
    int16_t x,y,a,b;
    skyrim_mobile_stick(0,.05f,.1f,&x,&y);assert(!x&&!y);
    skyrim_mobile_stick(1,1,.1f,&x,&y);assert(x>23000&&x<23200&&x==y);
    skyrim_mobile_stick(1,0,0,&x,&y);assert(x==32767&&!y);
    skyrim_mobile_stick(NAN,0,0,&x,&y);assert(!x&&!y);
    skyrim_mobile_camera(10,4,.02f,400,1.5f,0,&x,&y);
    skyrim_mobile_camera(30,12,.06f,400,1.5f,0,&a,&b);
    assert(abs(x-a)<=1&&abs(y-b)<=1&&x>0&&y<0);
    skyrim_mobile_camera(20,8,.02f,800,1.5f,1,&a,&b);
    assert(abs(x-a)<=1&&abs(y+b)<=1);
    skyrim_mobile_camera(INFINITY,0,.02f,400,1,0,&x,&y);assert(!x&&!y);
    skyrim_mobile_camera(10,4,0,400,1,0,&x,&y);assert(!x&&!y);
    skyrim_mobile_camera(0,0,.02f,400,1,0,&x,&y);assert(!x&&!y);
    skyrim_mobile_clear();
    skyrim_mobile_button(12,1);skyrim_mobile_button(0,1);skyrim_mobile_button(SKYRIM_PAD_RT,1);
    skyrim_mobile_move(.8f,0,.1f);skyrim_mobile_look(12,0,.02f,400,1.5f,0);
    SkyrimMobilePad p=skyrim_mobile_pad();assert(p.buttons==0x1001&&p.rt==255&&p.lt==0&&p.lx>0&&p.rx>0);
    skyrim_mobile_button(12,0);assert(skyrim_mobile_pad().buttons&0x1000); /* tap survives a slow poll */
    struct timespec delay={0,140000000};nanosleep(&delay,NULL);
    p=skyrim_mobile_pad();assert(p.buttons==1&&p.rx==0&&p.ry==0&&p.rt==255);
    skyrim_mobile_quicksave_key();assert(skyrim_mobile_f5_down());
    skyrim_mobile_clear();p=skyrim_mobile_pad();assert(!p.buttons&&!p.rt&&!p.lx&&!p.rx&&!skyrim_mobile_f5_down());
    skyrim_mobile_look(12,0,.02f,400,1,0);skyrim_mobile_look_end();assert(!skyrim_mobile_pad().rx);
    skyrim_mobile_set_limit(60,0);skyrim_mobile_set_thermal(3);assert(skyrim_mobile_fps_limit()==60);
    skyrim_mobile_set_limit(60,1);assert(skyrim_mobile_fps_limit()==24);
    skyrim_mobile_set_thermal(2);assert(skyrim_mobile_fps_limit()==30);
    skyrim_mobile_set_thermal(0);assert(skyrim_mobile_fps_limit()==60);
    skyrim_mobile_set_limit(24,1);skyrim_mobile_set_thermal(2);assert(skyrim_mobile_fps_limit()==24);
    SkyrimFrameClock clock={0};uint64_t t=1000000000;
    assert(skyrim_mobile_frame_deadline(&clock,t,30)==t);
    assert(skyrim_mobile_frame_deadline(&clock,t+10000000,30)==t+33333333);
    assert(skyrim_mobile_frame_deadline(&clock,t+40000000,30)==t+66666666);
    assert(skyrim_mobile_frame_deadline(&clock,t+2000000000,30)==t+2000000000); /* no catchup */
    assert(skyrim_mobile_frame_deadline(&clock,t+2010000000,60)==t+2010000000); /* change resets */
    assert(skyrim_mobile_frame_deadline(&clock,t+2010000001,0)==t+2010000001);
    pthread_t thread;assert(pthread_create(&thread,NULL,producer,NULL)==0);
    for(unsigned i=0;i<30000;i++) {p=skyrim_mobile_pad();assert(p.rt==0||p.rt==255);(void)skyrim_mobile_f5_down();}
    assert(pthread_join(thread,NULL)==0);skyrim_mobile_clear();
    puts("MOBILE_CORE input, multitouch composition, tap/expiry, F5, thermal policy, pacing, concurrent snapshots PASS");
}
