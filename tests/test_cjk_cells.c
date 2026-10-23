#include "ghostty_bridge.h"
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
ssize_t session_write_reply(session_t *s,const char *p,size_t n){(void)s;(void)p;return (ssize_t)n;}
static ghostty_bridge_cell_t grid[4][8];
static void visit(uint16_t x,uint16_t y,const ghostty_bridge_cell_t *c,void *u){(void)u;assert(x<8&&y<4);grid[y][x]=*c;}
static void capture(ghostty_bridge_t *b,ghostty_bridge_frame_t *f){assert(!ghostty_bridge_begin_frame(b,f));assert(!ghostty_bridge_visit_cells(b,0,visit,0));assert(!ghostty_bridge_finish_frame(b));}
static void write_text(ghostty_bridge_t *b,const char *s){ghostty_bridge_write(b,(const uint8_t*)s,strlen(s));}
int main(void){
 ghostty_bridge_t *b=0;ghostty_bridge_frame_t f;assert(!ghostty_bridge_create(&b,8,4,1000,0));
 write_text(b,"A中文B");capture(b,&f);
 assert(grid[0][0].columns==1&&grid[0][1].columns==2&&grid[0][2].columns==0);
 assert(grid[0][3].columns==2&&grid[0][4].columns==0&&grid[0][5].codepoint=='B');
 assert(f.cursor_x==6&&f.cursor_y==0);
 write_text(b,"\033[1;8H中");capture(b,&f);
 assert(grid[0][7].wide_tail&&grid[0][7].columns==1); /* wrap spacer still clears its cell */
 assert(grid[1][0].codepoint==0x4e2d&&grid[1][0].columns==2&&grid[1][1].columns==0);
 write_text(b,"\033[2;1HXY");capture(b,&f);
 assert(grid[1][0].codepoint=='X'&&grid[1][0].columns==1);
 assert(grid[1][1].codepoint=='Y'&&grid[1][1].columns==1);
 ghostty_bridge_destroy(b);puts("CJK_DOUBLE_COLUMNS_WRAP_AND_OVERWRITE_PASS");
}
