/* Authored host-only inputs. Does not load or execute a game. */
#include "mobile_core.h"
#include "shader_options.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static void *both_hands_producer(void *arg) {
    (void)arg;
    for (unsigned i=0;i<20000;i++) {
        skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,1);
        if (i&1) skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,0);
        else skyrim_mobile_button_cancel(SKYRIM_PAD_BOTH_HANDS);
        if (!(i%10)) skyrim_mobile_clear();
    }
    return NULL;
}
static void menu_touch(void) {
    const unsigned controls[]={SKYRIM_MENU_SETTINGS,SKYRIM_PAD_LB,SKYRIM_PAD_UP,SKYRIM_PAD_DOWN,SKYRIM_PAD_LEFT,SKYRIM_PAD_RIGHT,SKYRIM_PAD_BACK,
                              SKYRIM_MENU_EXPAND,SKYRIM_PAD_RB,SKYRIM_PAD_A,SKYRIM_PAD_B,SKYRIM_PAD_LT,SKYRIM_PAD_RT};
    const float safe[][2]={{832,420},{780,370},{568,320},{390,213}};
    for (unsigned i=0;i<sizeof(safe)/sizeof(safe[0]);i++) {
        SkyrimMenuButtonRect boxes[sizeof(controls)/sizeof(controls[0])];
        for (unsigned j=0;j<sizeof(controls)/sizeof(controls[0]);j++) {
            assert(skyrim_mobile_menu_rect(controls[j],safe[i][0],safe[i][1],&boxes[j]));
            SkyrimMenuButtonRect b=boxes[j];
            assert(b.x>=0 && b.y>=-.0001f && b.side>0 && b.x+b.side<=safe[i][0]+.0001f && b.y+b.side<=safe[i][1]+.0001f);
            if (safe[i][1]>=320) assert(b.side>=44 && b.y>=3 && b.y+b.side<=safe[i][1]-3);
            if (safe[i][1]>=370) assert(b.y>=20 && b.y+b.side<=safe[i][1]-20);
            for (unsigned k=0;k<j;k++) {
                SkyrimMenuButtonRect a=boxes[k];
                assert(a.x+a.side<=b.x || b.x+b.side<=a.x || a.y+a.side<=b.y || b.y+b.side<=a.y);
            }
        }
    }
    SkyrimMenuButtonRect r;
    assert(!skyrim_mobile_menu_rect(SKYRIM_PAD_MOVE,840,400,&r));
    assert(!skyrim_mobile_menu_rect(SKYRIM_PAD_A,0,400,&r));
    assert(!skyrim_mobile_menu_rect(SKYRIM_PAD_A,840,NAN,&r));
    assert(!skyrim_mobile_menu_rect(UINT32_MAX,840,400,&r));
    skyrim_mobile_clear();
    skyrim_mobile_button(SKYRIM_PAD_UP,1);
    skyrim_mobile_button_pulse(SKYRIM_PAD_UP);
    skyrim_mobile_button_pulse(SKYRIM_PAD_DOWN);
    assert(skyrim_mobile_pad().buttons==3);
    const struct timespec delay={0,150000000}; nanosleep(&delay,NULL);
    assert(skyrim_mobile_pad().buttons==1);
    skyrim_mobile_clear();
}
static void both_hands(void) {
    const struct timespec delay={0,140000000};
    SkyrimMobilePad p;
    /* One touch produces both trigger edges without setting Xbox button bits.
     * Preserve brief taps across a delayed game poll, like individual LT/RT. */
    skyrim_mobile_clear();
    skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,1);
    p=skyrim_mobile_pad(); assert(p.lt==255&&p.rt==255&&!p.buttons);
    skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,0);
    p=skyrim_mobile_pad(); assert(p.lt==255&&p.rt==255);
    nanosleep(&delay,NULL);
    p=skyrim_mobile_pad(); assert(!p.lt&&!p.rt);

    /* Holding lasts beyond the tap pulse; release either source independently.
     * Repeat for both individual hands to catch accidental source clearing. */
    const unsigned hands[]={SKYRIM_PAD_LT,SKYRIM_PAD_RT};
    for (unsigned i=0;i<2;i++) {
        skyrim_mobile_clear();
        skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,1);
        skyrim_mobile_button(hands[i],1);
        nanosleep(&delay,NULL);
        p=skyrim_mobile_pad(); assert(p.lt==255&&p.rt==255);
        skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,0);
        p=skyrim_mobile_pad(); assert(p.lt==(i==0?255:0)&&p.rt==(i==1?255:0));
        skyrim_mobile_button(hands[i],0);
        p=skyrim_mobile_pad(); assert(!p.lt&&!p.rt);

        skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,1);
        skyrim_mobile_button(hands[i],1);
        skyrim_mobile_button_cancel(hands[i]);
        p=skyrim_mobile_pad(); assert(p.lt==255&&p.rt==255);
        skyrim_mobile_button_cancel(SKYRIM_PAD_BOTH_HANDS);
        p=skyrim_mobile_pad(); assert(!p.lt&&!p.rt);
    }

    /* UIKit cancellation clears even an unexpired pulse but not another finger.
     * App inactivity / settings clear every source, including the new control. */
    skyrim_mobile_button(SKYRIM_PAD_LT,1);
    skyrim_mobile_button(SKYRIM_PAD_RT,1);
    skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,1);
    skyrim_mobile_button_cancel(SKYRIM_PAD_BOTH_HANDS);
    p=skyrim_mobile_pad(); assert(p.lt==255&&p.rt==255);
    skyrim_mobile_button_cancel(SKYRIM_PAD_LT);
    p=skyrim_mobile_pad(); assert(!p.lt&&p.rt==255);
    skyrim_mobile_button_cancel(SKYRIM_PAD_RT);
    p=skyrim_mobile_pad(); assert(!p.lt&&!p.rt);
    skyrim_mobile_button(SKYRIM_PAD_BOTH_HANDS,1);
    skyrim_mobile_clear();
    p=skyrim_mobile_pad(); assert(!p.lt&&!p.rt&&!p.buttons);
    /* Move retains saved layout ID18 and must never become a gamepad bit. */
    skyrim_mobile_button(SKYRIM_PAD_MOVE,1);
    skyrim_mobile_button(SKYRIM_PAD_CONTROL_COUNT,1);
    skyrim_mobile_button(UINT32_MAX,1);
    skyrim_mobile_button_cancel(UINT32_MAX);
    p=skyrim_mobile_pad(); assert(!p.lt&&!p.rt&&!p.buttons);

    pthread_t thread;
    assert(pthread_create(&thread,NULL,both_hands_producer,NULL)==0);
    for (unsigned i=0;i<30000;i++) {
        p=skyrim_mobile_pad(); assert(p.lt==p.rt&&!p.buttons);
    }
    assert(pthread_join(thread,NULL)==0);
    skyrim_mobile_clear();
}
int main(void) {
    int width = 1179, height = 2556;
    skyrim_mobile_landscape_size(&width, &height);
    assert(width == 2556 && height == 1179);
    skyrim_mobile_landscape_size(&width, &height);
    assert(width == 2556 && height == 1179);
    width = 0;
    skyrim_mobile_landscape_size(&width, &height);
    assert(width == 0 && height == 1179);
    assert(skyrim_mobile_choose_render_size(2868, 1320, 720, &width, &height));
    assert(width == 1564 && height == 720);
    assert(skyrim_mobile_choose_render_size(1320, 2868, 2160, &width, &height));
    assert(width == 2868 && height == 1320);
    assert(skyrim_mobile_choose_render_size(956, 440, 720, &width, &height));
    assert(width == 956 && height == 440);
    assert(!skyrim_mobile_choose_render_size(2868, 1320, 0, &width, &height));
    assert(width == 956 && height == 440);
    SkyrimMobileViewport viewport = skyrim_mobile_viewport(2868, 1320, 1920, 1080);
    assert(viewport.x == 261 && viewport.y == 0 && viewport.width == 2346 && viewport.height == 1320);
    double x_norm = -1, y_norm = -1;
    assert(!skyrim_mobile_viewport_point(viewport, 2868, 1320, .01, .5, &x_norm, &y_norm));
    assert(x_norm == -1 && y_norm == -1);
    assert(skyrim_mobile_viewport_point(viewport, 2868, 1320, .5, .5, &x_norm, &y_norm));
    assert(x_norm == .5 && y_norm == .5);
    viewport = skyrim_mobile_viewport(1920, 1080, 2868, 1320);
    assert(viewport.x == 0 && viewport.y == 98 && viewport.width == 1920 && viewport.height == 883);
    assert(!skyrim_mobile_viewport_point(viewport, 1920, 1080, .5, .98, NULL, NULL));
    assert(!skyrim_mobile_viewport_point(viewport, 1920, 1080, NAN, .5, NULL, NULL));
    viewport = skyrim_mobile_viewport(2868, 1320, 956, 440);
    assert(viewport.x == 0 && viewport.y == 0 && viewport.width == 2868 && viewport.height == 1320);
    assert(!skyrim_mobile_viewport_point(viewport, 2868, 1320, 1, .5, NULL, NULL));
    /* Append-only startup preference: missing old dictionaries remain OFF.
     * Existing five IDs keep their persisted meaning. */
    assert(SKYRIM_SHADER_REUSE == 4 && SKYRIM_SHADER_EXACT_STATE == 5);
    assert(SKYRIM_SHADER_CPU_SAMPLE == 6 && SKYRIM_SHADER_DISPLAY_SYNC == 7 && SKYRIM_SHADER_OPTION_COUNT == 8);
    assert(!strcmp(skyrim_shader_option_key(5), "shaderExactState"));
    assert(!strcmp(skyrim_shader_option_environment(5), "SKYRIM_EXACT_GRAPHICS_STATE"));
    assert(!strcmp(skyrim_shader_option_key(6), "cpuSample"));
    assert(!strcmp(skyrim_shader_option_environment(6), "SKYRIM_IOS_CPU_SAMPLE"));
    assert(!strcmp(skyrim_shader_option_key(7), "displaySync"));
    assert(!strcmp(skyrim_shader_option_environment(7), "SKYRIM_IOS_DISPLAY_SYNC"));
    assert(!skyrim_shader_option_key(8) && !skyrim_shader_option_environment(UINT32_MAX));
    assert(skyrim_shader_option_enabled(1));
    assert(!skyrim_shader_option_enabled(0) && !skyrim_shader_option_enabled(NAN));
    assert(!skyrim_shader_option_enabled(2) && !skyrim_shader_option_enabled(-1));
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
    /* Continuous 60/120/240 Hz delivery keeps exactly the original vector.
     * A stationary finger no longer carries a full 80 ms turn. */
    const uint64_t periods[]={4166667,8333333,16666667,33333333};
    for(unsigned j=0;j<sizeof periods/sizeof periods[0];++j) {
        skyrim_mobile_camera(10,4,.02f,400,1.5f,0,&x,&y);
        uint64_t interval=periods[j]<8000000 ? 8000000 : periods[j]>34000000 ? 34000000 : periods[j];
        uint64_t hold=interval+8000000;
        skyrim_mobile_camera_age(x,y,0,periods[j],&a,&b);assert(a==x&&b==y);
        skyrim_mobile_camera_age(x,y,periods[j],periods[j],&a,&b);assert(a==x&&b==y);
        skyrim_mobile_camera_age(x,y,hold,periods[j],&a,&b);assert(a==x&&b==y);
        skyrim_mobile_camera_age(x,y,hold+interval/2,periods[j],&a,&b);
        assert(a>0&&a<x&&b<0&&b>y);
        /* Above-deadzone speed halves, including a diagonal turn. */
        float old=hypotf(x,y)/32767.0f-.27f,newv=hypotf(a,b)/32767.0f-.27f;
        assert(fabsf(newv/old-.5f)<.001f);
        skyrim_mobile_camera_age(x,y,hold+interval,periods[j],&a,&b);assert(!a&&!b);
        skyrim_mobile_camera_age(x,y,UINT64_MAX,periods[j],&a,&b);assert(!a&&!b);
    }
    skyrim_mobile_camera_age(0,0,30000000,16666667,&a,&b);assert(!a&&!b);
    skyrim_mobile_camera_age(1,-1,30000000,16666667,&a,&b);assert(a==1&&b==-1);
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
    both_hands();
    menu_touch();
    puts("MOBILE_CORE input, multitouch composition, tap/expiry, F5, thermal policy, pacing, both hands/cancel, concurrent snapshots PASS");
}
