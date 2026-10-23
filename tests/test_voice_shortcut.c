#include "voice_shortcut.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
    voice_shortcut_t s={0};
    key_event_t down={.sym='0',.unicode='0',.keycode='0',.pressed=1},up=down,repeat=down,other=down;
    up.pressed=0;repeat.repeat=1;other.sym='x';
    assert(voice_shortcut_key(&s,&down,11,0,1,100)==1);
    assert(voice_shortcut_key(&s,&up,11,0,1,200)==3); /* tap, one replay */
    assert(!voice_shortcut_tick(&s,11,0,1,900));
    assert(voice_shortcut_key(&s,&down,11,0,1,1000)==1);
    assert(voice_shortcut_key(&s,&repeat,11,0,1,1100)==1);
    assert(!voice_shortcut_tick(&s,11,0,1,1599));
    assert(voice_shortcut_tick(&s,11,0,1,1600)==4); /* no repeat required */
    assert(!voice_shortcut_tick(&s,11,0,1,5000));
    assert(voice_shortcut_key(&s,&repeat,11,0,0,5000)==1);
    assert(voice_shortcut_key(&s,&up,11,0,0,5100)==1);
    assert(voice_shortcut_key(&s,&down,11,0,1,6000)==1);
    assert(voice_shortcut_key(&s,&up,11,0,1,6600)==5); /* overdue release */
    assert(voice_shortcut_key(&s,&down,11,0,1,7000)==1);
    assert(voice_shortcut_key(&s,&other,11,0,1,7100)==2); /* ordered 0,x */
    assert(s.down.sym=='0'&&s.down.pressed&&!s.down.repeat);
    assert(voice_shortcut_key(&s,&up,11,1,1,7200)==1);
    for(int reason=0;reason<4;reason++){
        assert(voice_shortcut_key(&s,&down,11,0,1,8000)==1);
        if(reason==3)voice_shortcut_cancel(&s);
        assert(!voice_shortcut_tick(&s,reason==0?25:11,reason==1?1:0,reason!=2,9000));
        assert(voice_shortcut_key(&s,&up,11,0,1,9100)==1); /* no stale replay */
    }
    for(int mod=1;mod<=4;mod*=2){
        down.modifiers=mod;assert(!voice_shortcut_key(&s,&down,11,0,1,10000));
    }
    down.modifiers=0;
    assert(!voice_shortcut_key(&s,&down,11,0,0,10000)); /* editor/UI owns zero */
    assert(voice_shortcut_key(&s,&down,11,0,1,11000)==1);
    voice_shortcut_cancel(&s); /* lost keyup on focus loss */
    assert(voice_shortcut_key(&s,&down,11,0,1,12000)==1);
    assert(voice_shortcut_key(&s,&up,11,0,1,12050)==3);
    puts("voice shortcut: tap, timer hold, repeat ownership, input ordering, modifiers, context loss PASS");
    return 0;
}
