#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
int main(int argc,char* argv[]){
	char warn_no[] = "please enter a number\n";
	char out[] = "nothing happened for a while\n";
	char warn_too_many[] = "too many arguments\n";
	if(argc==1)
		write(1,warn_no,sizeof(warn_no));
	else if(argc>2)
		write(1,warn_too_many,sizeof(warn_too_many));
	else{
		sleep(atoi(argv[1]));
		write(1,out,sizeof(out));
	}
	exit(0);
}
