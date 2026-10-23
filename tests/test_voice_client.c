/* Native protocol/commit harness. All transcript text is synthetic test data.
 * The pairing key is read from a private file, never emitted or passed in argv. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "voice_client.h"
#include "voice_commit.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
extern unsigned voice_fake_opened(void),voice_fake_closed(void),voice_fake_bytes(void);
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static double seconds(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec/1e9;}
typedef struct {voice_client_t *client;atomic_int done;} observer_t;
static void *observe(void *arg){
    observer_t *o=arg;
    while(!atomic_load(&o->done)){
        voice_snapshot_t s;voice_client_snapshot(o->client,&s);
        assert(s.state>=VOICE_IDLE&&s.state<=VOICE_INSERTING);
        const char *end=memchr(s.text,0,sizeof(s.text));
        assert(end&&memchr(s.error,0,sizeof(s.error)));
        /* Shorter rewrites must not leave earlier transcript bytes in a
         * published snapshot beyond its terminating NUL. */
        for(const char *p=end+1;p<s.text+sizeof(s.text);++p)assert(!*p);
        assert(voice_client_audio_level(o->client)<=1000);
        struct timespec delay={0,100000};nanosleep(&delay,NULL);
    }return NULL;
}
typedef struct {voice_client_t *client;const char *key;int port,probe,result;atomic_int *ready,*go;} starter_t;
static void *start_together(void *arg){
    starter_t *s=arg;atomic_fetch_add(s->ready,1);
    while(!atomic_load(s->go)){struct timespec delay={0,100000};nanosleep(&delay,NULL);}
    s->result=voice_client_start(s->client,"127.0.0.1",s->port,s->key,s->probe);return NULL;
}
typedef struct {char got[VOICE_TEXT_MAX+13];size_t n,calls;int mode;} sink_t;
static ssize_t write_sink(void *data,const char *bytes,size_t count){
    sink_t *s=data;size_t call=s->calls++;
    if(s->mode==1)return 0;
    if(s->mode==2){errno=EIO;return -1;}
    if(s->mode==3)return (ssize_t)count+1;
    if(call==1||call==2||call==4){errno=call==1?EINTR:call==2?EAGAIN:EWOULDBLOCK;return -1;}
    size_t take=call==0?2:call==3?1:count;if(take>count)take=count;
    CHECK(s->n+take<=sizeof(s->got));memcpy(s->got+s->n,bytes,take);s->n+=take;return (ssize_t)take;
}
static void abort_tail_tests(int bracketed){
    /* Explicit complete-codepoint prefixes form an oracle independent of the
     * suffix routine's continuation-byte scan. Includes 1/2/3/4-byte UTF-8. */
    const char *prefixes[]={"","中","中🙂","中🙂A","中🙂Aé","中🙂Aé文","中🙂Aé文 ","中🙂Aé文 Z"};
    const size_t prefix_count=sizeof(prefixes)/sizeof(*prefixes);
    const char *whole=prefixes[prefix_count-1];size_t body_length=strlen(whole);
    voice_commit_t commit;CHECK(!voice_commit_prepare(&commit,"中🙂Aé文\nZ",bracketed));
    for(size_t offset=0;offset<=commit.size;offset++){
        commit.offset=offset;voice_commit_t original=commit;
        struct {unsigned char before[8];char tail[16];unsigned char after[8];} bounded;
        memset(&bounded,0xa5,sizeof(bounded));
        size_t size=voice_commit_abort_tail(&commit,bounded.tail);CHECK(size<=sizeof(bounded.tail));
        CHECK(!memcmp(&commit,&original,sizeof(commit)));
        for(size_t i=0;i<8;i++)CHECK(bounded.before[i]==0xa5&&bounded.after[i]==0xa5);
        for(size_t i=size;i<sizeof(bounded.tail);i++)CHECK((unsigned char)bounded.tail[i]==0xa5);
        if(!offset||offset==commit.size)CHECK(size==0);
        /* The emergency suffix must itself survive interrupted/short writes,
         * and a completed drain must never replay any suffix bytes. */
        voice_commit_t emergency={0};memcpy(emergency.bytes,bounded.tail,size);emergency.size=size;
        sink_t sink={0};int drained=0;
        for(int i=0;i<20&&!drained;i++){drained=voice_commit_drain(&emergency,write_sink,&sink);CHECK(drained>=0);}
        CHECK(drained==1&&sink.n==size&&!memcmp(sink.got,bounded.tail,size));
        size_t calls=sink.calls;CHECK(voice_commit_drain(&emergency,write_sink,&sink)==1&&sink.calls==calls);
        char delivered[80],expected[80];size_t delivered_size=offset+size,expected_size=0;
        CHECK(delivered_size<=sizeof(delivered));memcpy(delivered,commit.bytes,offset);memcpy(delivered+offset,sink.got,size);
        size_t received_body=bracketed?(offset<=6?0:offset-6):offset;
        if(received_body>body_length)received_body=body_length;
        size_t complete=0;
        while(complete+1<prefix_count&&strlen(prefixes[complete])<received_body)complete++;
        size_t expected_body=strlen(prefixes[complete]);
        if(offset){
            if(bracketed){memcpy(expected,"\033[200~",6);expected_size=6;}
            memcpy(expected+expected_size,prefixes[complete],expected_body);expected_size+=expected_body;
            if(bracketed){memcpy(expected+expected_size,"\033[201~",6);expected_size+=6;}
        }
        CHECK(delivered_size==expected_size&&!memcmp(delivered,expected,expected_size));
        if(offset){
            CHECK(voice_text_valid(delivered+(bracketed?6:0),expected_body));
            if(bracketed){
                int begins=0,ends=0;
                for(size_t i=0;i+6<=delivered_size;i++){begins+=!memcmp(delivered+i,"\033[200~",6);ends+=!memcmp(delivered+i,"\033[201~",6);}
                CHECK(begins==1&&ends==1);
            }
        }
    }
    char untouched[16];memset(untouched,0x5a,sizeof(untouched));commit.offset=commit.size+1;
    CHECK(!voice_commit_abort_tail(&commit,untouched));CHECK(!voice_commit_abort_tail(NULL,untouched));
    for(size_t i=0;i<sizeof(untouched);i++)CHECK(untouched[i]==0x5a);
}
static void unit_tests(void){
    CHECK(voice_text_valid("中文🙂\n\tASCII",strlen("中文🙂\n\tASCII")));
    const char *bad[]={"\x1b[2J","\r","\x7f","\xc2\x85","\x80","\xc0\xaf","\xe0\x80\xaf","\xed\xa0\x80","\xf0\x80\x80\xaf","\xf4\x90\x80\x80","\xf5\x80\x80\x80","\xe4\xb8"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(!voice_text_valid(bad[i],strlen(bad[i])));
    CHECK(!voice_text_valid("a\0b",3));
    CHECK(voice_text_valid("\xc2\xa0\xef\xbf\xbf\xf4\x8f\xbf\xbf",9));
    voice_commit_t commit={0};CHECK(!voice_commit_prepare(&commit,"中文\nhello\t🙂",0));
    CHECK(commit.size==strlen("中文 hello 🙂")&&!memcmp(commit.bytes,"中文 hello 🙂",commit.size));
    CHECK(!memchr(commit.bytes,'\r',commit.size)&&!memchr(commit.bytes,'\n',commit.size)&&!memchr(commit.bytes,'\t',commit.size));
    sink_t sink={0};int rc=0;size_t previous=0;
    for(int i=0;i<20&&!rc;i++){rc=voice_commit_drain(&commit,write_sink,&sink);CHECK(rc>=0&&commit.offset>=previous&&commit.offset<=commit.size);previous=commit.offset;}
    CHECK(rc==1&&sink.n==commit.size&&!memcmp(sink.got,commit.bytes,sink.n));
    size_t calls=sink.calls;CHECK(voice_commit_drain(&commit,write_sink,&sink)==1&&sink.calls==calls);
    CHECK(!voice_commit_prepare(&commit,"文字\n第二行",1));
    CHECK(commit.size==strlen("\033[200~文字 第二行\033[201~"));CHECK(!memcmp(commit.bytes,"\033[200~文字 第二行\033[201~",commit.size));
    for(int mode=1;mode<=3;mode++){sink=(sink_t){.mode=mode};CHECK(voice_commit_drain(&commit,write_sink,&sink)==-1&&commit.offset==0);}
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(voice_commit_prepare(&commit,bad[i],1)<0);
    CHECK(voice_commit_prepare(&commit,"",0)<0);CHECK(voice_commit_prepare(NULL,"x",0)<0);CHECK(voice_commit_prepare(&commit,NULL,0)<0);
    char text[VOICE_TEXT_MAX+2];memset(text,'x',sizeof(text));text[VOICE_TEXT_MAX]=0;
    CHECK(!voice_commit_prepare(&commit,text,1)&&commit.size==VOICE_TEXT_MAX+12);
    text[VOICE_TEXT_MAX]='x';text[VOICE_TEXT_MAX+1]=0;CHECK(voice_commit_prepare(&commit,text,0)<0);
    voice_client_t *v=voice_client_create();CHECK(v);
    CHECK(!voice_client_audio_level(NULL));CHECK(!voice_client_audio_level(v));
    const char *synthetic="0000000000000000000000000000000000000000000000000000000000000000";
    CHECK(voice_client_start(v,"localhost",45850,synthetic,0)<0);CHECK(voice_client_start(v,"127.0.0.1",0,synthetic,0)<0);
    CHECK(voice_client_start(v,"127.0.0.1",45850,"short",0)<0);CHECK(voice_client_start(v,"127.0.0.1",45850,"g00000000000000000000000000000000000000000000000000000000000000000",0)<0);
    voice_client_destroy(v);abort_tail_tests(0);abort_tail_tests(1);printf("voice native unit: %u checks passed\n",checks);
}
static void report(voice_client_t *v,const voice_snapshot_t *s,int requested){
    printf("{\"requested\":%d,\"state\":%d,\"revision\":%u,\"change\":%u,\"level\":%u,\"error\":\"%s\",\"text_hex\":\"",requested,s->state,s->revision,s->change,voice_client_audio_level(v),s->error);
    for(const unsigned char *p=(const unsigned char*)s->text;*p;p++)printf("%02x",*p);
    printf("\",\"opens\":%u,\"closes\":%u,\"bytes\":%u}\n",voice_fake_opened(),voice_fake_closed(),voice_fake_bytes());fflush(stdout);
}
int main(int argc,char **argv){
    if(argc==1){unit_tests();return 0;}
    /* Commands use select(2); stdio must not hide a coalesced second command
     * in its private buffer after the pipe descriptor has become empty. */
    CHECK(!setvbuf(stdin,NULL,_IONBF,0));
    CHECK(argc==4);int port=atoi(argv[1]),probe=atoi(argv[3]);
    char key[66]={0};FILE *f=fopen(argv[2],"r");CHECK(f&&fgets(key,sizeof(key),f));fclose(f);key[strcspn(key,"\r\n")]=0;
    voice_client_t *v=voice_client_create();CHECK(v);CHECK(!voice_client_start(v,"127.0.0.1",port,key,probe));
    observer_t observer={.client=v};atomic_init(&observer.done,0);
    pthread_t observer_thread;CHECK(!pthread_create(&observer_thread,NULL,observe,&observer));
    double deadline=seconds()+30;unsigned change=0;int quit=0;
    while(!quit&&seconds()<deadline){
        voice_snapshot_t s;voice_client_snapshot(v,&s);if(s.change!=change){report(v,&s,0);change=s.change;}
        fd_set rd;FD_ZERO(&rd);FD_SET(STDIN_FILENO,&rd);struct timeval tv={0,10000};
        int ready=select(STDIN_FILENO+1,&rd,NULL,NULL,&tv);if(ready<0&&errno==EINTR)continue;CHECK(ready>=0);
        if(ready){char command[32];if(!fgets(command,sizeof(command),stdin))break;
            if(!strcmp(command,"STOP\n"))voice_client_stop(v);
            else if(!strcmp(command,"CANCEL\n"))voice_client_cancel(v);
            else if(!strcmp(command,"SNAP\n")){voice_client_snapshot(v,&s);report(v,&s,1);}
            else if(!strcmp(command,"START\n")){printf("{\"start_result\":%d}\n",voice_client_start(v,"127.0.0.1",port,key,probe));fflush(stdout);}
            else if(!strcmp(command,"START_RACE\n")){
                atomic_int ready=0,go=0;pthread_t threads[2];
                starter_t starters[2]={{v,key,port,probe,99,&ready,&go},{v,key,port,probe,99,&ready,&go}};
                for(int i=0;i<2;i++)CHECK(!pthread_create(&threads[i],NULL,start_together,&starters[i]));
                while(atomic_load(&ready)!=2){struct timespec delay={0,100000};nanosleep(&delay,NULL);}
                atomic_store(&go,1);for(int i=0;i<2;i++)CHECK(!pthread_join(threads[i],NULL));
                printf("{\"race_results\":[%d,%d]}\n",starters[0].result,starters[1].result);fflush(stdout);
            }
            else if(!strcmp(command,"QUIT\n"))quit=1;
            else CHECK(!"unknown harness command");
        }
    }
    atomic_store(&observer.done,1);CHECK(!pthread_join(observer_thread,NULL));
    double start=seconds();voice_client_destroy(v);memset(key,0,sizeof(key));
    printf("{\"end\":true,\"opens\":%u,\"closes\":%u,\"bytes\":%u,\"destroy_ms\":%.1f}\n",voice_fake_opened(),voice_fake_closed(),voice_fake_bytes(),(seconds()-start)*1000);
    return quit?0:2;
}
