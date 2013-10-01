/*
    BigBrother Daemon
    Andrew Filonov <andrew.filonov@gmail.com>
    (C) 2013
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>

#define NUM_OF_EVENTS 5
#define MESSAGE_MAX 1000
#define VERSION "0.1"


int controlSocket;
char defSocketName[]="/var/run/bbw.sock";
char *socketName=defSocketName;
int kq;
int daemonFlag=1;
int listenFlag=0;
int debugFlag=1;
struct sockaddr_un address;
socklen_t addrLen;


typedef struct{
  LIST_ENTRY(entry) entries;
  int pid;
  int needWait;
  char cmd[1];
} procinfo;   
LIST_HEAD(listhead, procinfo) head =
         LIST_HEAD_INITIALIZER(head);
/*
--------------------------------------------------------------------------------
 */
void usage(void){
  puts("usage: bbd [-l] [-f] [-c <command>] [-d] [-s <socket name>]");
  puts(" -f - force to run as foreground process");
  puts(" -l - server mode - listen to control socket");
  puts(" -d - debug mode");
  puts(" -s <socket> - path to UNIX socket for client/server");
  puts(" -c <command> - command for daemon in client mode");
  puts("    same as: echo cmd | nc -w 1 -u -U /path/to/socket");

}
/*
--------------------------------------------------------------------------------
 */
void checkFd(int fd,char* str){
  if(-1 == fd){
    syslog(LOG_ERR,str,strerror(errno));
    exit(1);
  }
}
/*
--------------------------------------------------------------------------------
*/
void fillAddr(){
  memset(&address, 0, sizeof(struct sockaddr_un));
  address.sun_family = AF_UNIX;
  strlcpy(address.sun_path, socketName,sizeof(address.sun_path));
  addrLen=sizeof(address.sun_family)+strlen(address.sun_path)+1;
}
/*
--------------------------------------------------------------------------------
 */
void client(int p,char *cmd){
  char buff[MESSAGE_MAX+1];
  int ret;

  // create socket
  controlSocket = socket(PF_LOCAL,SOCK_DGRAM,0);
  checkFd(controlSocket,"Can't create control socket: %s");

  fillAddr();
  syslog(LOG_DEBUG ,"Use control socket: \"%s\"",address.sun_path);

  syslog(LOG_INFO,"send command to daemon:%s",buff);
  snprintf(buff,MESSAGE_MAX,"%d %s",p,cmd);
  ret=sendto(controlSocket,buff,strlen(buff),0,(struct sockaddr *)&address,addrLen);

  //syslog(LOG_DEBUG ,"Ret code: \"%d\" %d  %s",ret,errno,strerror(errno));
  return;
}

/*
--------------------------------------------------------------------------------
 */
pid_t execCmd(char *cmd){
  pid_t pid;
  pid=fork();
  if(0 == pid){
    execl("/bin/sh","/bin/sh","-c",cmd,NULL);
    syslog(LOG_ERR,"Exec %s failed",cmd);
    exit(1);
  }
  return pid;
}
/*
--------------------------------------------------------------------------------
 */
procinfo* watchForPID(int pid,int needWait,char* cmd){
  int len=sizeof(procinfo);
  int cmdlen=0;
  procinfo *pi;
  struct kevent ev;
  int ret;

  if(cmd){
    cmdlen=strlen(cmd);
    len+=cmdlen;
  }
  pi=malloc(len);
  if(NULL == pi){
    syslog(LOG_ALERT,"Can't allocate memory");
    return NULL;
  }
  memset(pi, 0, len);
  pi->pid=pid;
  pi->needWait=needWait;
  if(cmd) strlcpy(pi->cmd,cmd,ret+1);

  EV_SET(&ev,pid,EVFILT_PROC,EV_ADD| EV_ENABLE ,NOTE_EXIT,0,pi);
  syslog(LOG_DEBUG,"Set UDATA pid: %u cmd: %s needwait:%d",
	 pi->pid,pi->cmd,pi->needWait);
  syslog(LOG_DEBUG,"AddEvent: ident:%u filter:%hu  fflags:%x data: %p udata: %p",
	 ev.ident,ev.filter,ev.fflags,ev.data,ev.udata);
  ret=kevent(kq,&ev,1,NULL,0,NULL);
  if( -1 == ret){
    syslog(LOG_WARNING,"bad PID - filter is not set",pid);
    free(pi);
  } 
}
/*
--------------------------------------------------------------------------------
 */
void parseCmd(char *buff){
  char cmd[MESSAGE_MAX+5];
  int pid;
  int ret;

  memset(cmd, 0, sizeof(cmd));
  ret=sscanf(buff,"%d %[^\r\n]",&pid,cmd);
  if(2 == ret){
    syslog(LOG_DEBUG,"PID: %d : %s",pid,cmd);
    watchForPID(pid,0,cmd);
  } else {
    syslog(LOG_WARNING,"parse failed:%s",buff);
  }
}
/*
--------------------------------------------------------------------------------
 */
void receiveMessage(void){
  char buff[MESSAGE_MAX+1];
  ssize_t sz;


  sz=recvfrom(controlSocket,buff,MESSAGE_MAX,0,(struct sockaddr *)&address,&addrLen);
  buff[sz]='\0';
  syslog(LOG_DEBUG,"Receive %d bytes from control socket",sz);
  parseCmd(buff);
}
/*
--------------------------------------------------------------------------------
 */void eventLoop(void){
  struct kevent chlist[NUM_OF_EVENTS];   /* events we want to monitor */
  struct kevent evlist[NUM_OF_EVENTS];   /* events that were triggered */
  int evCount;
  int chCount=1;
  int i;
  int ret;
  int status=-2;
  procinfo *pi;


  EV_SET(chlist,controlSocket,EVFILT_READ,EV_ADD| EV_ENABLE,0,0,NULL);
  for(;;){
    syslog(LOG_DEBUG,"Waiting for events...");
    evCount=kevent(kq,chlist,chCount,evlist,NUM_OF_EVENTS,NULL);
    chCount = 0;
    syslog(LOG_DEBUG,"receive %d events",evCount);
    checkFd(evCount,"kevent error");
    for(i=0;i<evCount;i++){
      if(evlist[i].ident=controlSocket && evlist[i].udata == NULL){
	receiveMessage();
      } else {
	syslog(LOG_DEBUG,"Event: ident:%u filter:%hu  fflags:%x data: %p udata: %p",
	       evlist[i].ident,evlist[i].filter,evlist[i].fflags,evlist[i].data,evlist[i].udata);
	pi = (procinfo*)evlist[i].udata;
	syslog(LOG_DEBUG,"Recc UDATA pid: %u cmd: %s needwait:%d",
	       pi->pid,pi->cmd,pi->needWait);
	
	if(pi->needWait){
	  ret = waitpid(pi->pid,&status,WEXITED|WNOHANG);
	  syslog(LOG_DEBUG,"Waitpid: %u return: %d status:%d",pi->pid,ret,status);
	}
	if(*pi->cmd){
	  ret= execCmd(pi->cmd);
	  if( 0 < ret){ // we should wait for our childs
	    watchForPID(ret,1,NULL);
	  }
	}
	free(pi);
      }
    }
  }
}
/*
--------------------------------------------------------------------------------
 */
void server(void){
  int ret;

  if(daemonFlag){
    daemon(0,0);
  }
  // create kqueue
  kq=kqueue();
  checkFd(kq,"Can't create kqueue: %s");

  // create socket
  controlSocket = socket(PF_LOCAL,SOCK_DGRAM,0);
  checkFd(controlSocket,"Can't create control socket: %s");

  ret=unlink(socketName);
  if (-1 == ret) {
    syslog(LOG_NOTICE,"Can't remove control socket \"%s\": %s",socketName,strerror(errno));
  }
  fillAddr();
  syslog(LOG_DEBUG ,"Use control socket: \"%s\"",address.sun_path);
 
  ret = bind(controlSocket,(struct sockaddr *)&address,
	     sizeof(address.sun_family)+strlen(address.sun_path)+1);
  checkFd(ret,"Can't bind to control socket: %s");

  eventLoop();
}
/*
--------------------------------------------------------------------------------
 */
int main(int argc,char **argv){
  int ch;
  int logopt=LOG_PID;
  int logmask=LOG_UPTO(LOG_NOTICE);
  int pid=-1;
  char *cmd=NULL;
  umask(077);
  while ((ch = getopt(argc, argv, "lfdc:s:p:")) != -1) {
    switch (ch) {
    case 'l': // listen
      listenFlag=1;
      logopt|=LOG_PERROR;
      break;
    case 'f': // no fork
      daemonFlag=0;
      logopt|=LOG_PERROR;
      break;
    case 'd': // debug
      debugFlag=1;
      logmask=LOG_UPTO(LOG_DEBUG);
      break;
    case 'c': // command for server
      cmd=optarg;
      break;
    case 'p': // command for server
      pid=atoi(optarg);
      break;
    case 's': // socket name
      socketName=optarg;
      break;
    default:
      usage();
      return 0;
    }
  }
  
  openlog("shepherd",logopt,LOG_DAEMON);
  setlogmask(logmask);
  syslog(LOG_DEBUG,"Started");
  if(listenFlag){
    server();
  } else {
    if(cmd ||  (-1 < pid) ) {
      client(pid,cmd);
    } else {
      usage();
    }
  }
  argc -= optind;
  argv += optind;
  return 0;
}

