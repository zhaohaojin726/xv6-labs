#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#define MAXNUM 128
#define MAXLEN 1024
int main(int argc,char* argv[]){
	char argu[MAXLEN];
	char* sub_argv[MAXNUM];
	int c;
	for(c=1;argv[c]!=0;c++)
		sub_argv[c-1]=argv[c];
	char all_arg[MAXLEN];
	read(0,all_arg,MAXLEN);
	int k=0;
	while(all_arg[k]!=0){
		int count=0;
		while(all_arg[k]!='\n' && all_arg[k]!=0)
			argu[count++]=all_arg[k++];
		k++;
		argu[count]=0;
		int head=0,cnt=0;
		int i=0;
		for(int p=c;sub_argv[p]!=0;p++){
			free(sub_argv[p]);
			sub_argv[p]=0;
		}
		for(;argu[i]!='\n' && argu[i]!=0;i++){
			if(argu[i]==' '){
				sub_argv[argc-1+cnt]=malloc(MAXNUM);
				memmove(sub_argv[argc-1+cnt++],argu+head,i-head);
				head=i+1;
			}
		}
	
		
		sub_argv[argc-1+cnt]=malloc(MAXNUM);
		memmove(sub_argv[argc-1+cnt],argu+head,i-head);
		if(fork()==0){
			exec(argv[1],sub_argv);
		}
		else{
			wait(0);
		}
		memset(argu,0,MAXLEN);
	}
	return 0;
}
