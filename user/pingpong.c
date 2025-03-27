#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
                    
int main(int argc,char *argv[]){
	if(argc!=1)
		printf("this program need no argument\n");	
        int par_to_son[2];
     	int son_to_par[2];	
        pipe(par_to_son); 
     	pipe(son_to_par);	
	char buf[1];
        if(fork()==0){
                int pid_son=getpid();
		close(par_to_son[1]);
		read(par_to_son[0],buf,sizeof(buf));
		close(par_to_son[0]);
		printf("%d: received ping\n",pid_son);
		close(son_to_par[0]);
		write(son_to_par[1],buf,1);
		close(son_to_par[1]);
              	exit(0);
        }           
        else{       
                int pid_par=getpid();
                close(par_to_son[0]);
		write(par_to_son[1],"p",1);
		close(par_to_son[1]);
		wait(0);
		close(son_to_par[1]);
		read(son_to_par[0],buf,1);
                close(son_to_par[0]);
                printf("%d: received pong\n",pid_par);
                exit(0);
	}
}
