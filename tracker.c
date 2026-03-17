
#include <stdio.h>
#include <sys/types.h> //provides pid_t 
#include <sys/socket.h> //provides socket functions 
#include <errno.h> //provides error codes 
#include <sys/dir.h> //directory scanning functions when implementing LIST to scan torrents 
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h> 
#include <string.h> 
#include <unistd.h>




#define MAXLINE 512
#define MAXREQUESTS 20
void *peer_handler(void *arg);
void handle_list_req(int sock_child){}
void handle_get_req(int sock_child, char *fname){}
void xtrct_fname(char *msg, char *delim){}
void tokenize_createmsg(char *msg){}
void handle_createtracker_req(int sock_child){}
void tokenize_updatemsg(char *msg){}
void handle_updatetracker_req(int sock_child){}


int main(){
	int sockid; 
    int sock_child;
    int server_port;
    socklen_t clilen;


	//will read the sconfig file to determine where to put files 
	FILE *fptr; 
	fptr = fopen("sconfig","r");
	if(fptr == NULL){
    printf("Cannot open sconfig\n");
    exit(0);
}
	char port_number[256]; 
	char shared_directory[256];  
	fgets(port_number,sizeof(port_number),fptr); 
	fgets(shared_directory,sizeof(shared_directory),fptr); 
	server_port = atoi(port_number); 
	fclose(fptr); 

	clilen = sizeof(struct sockaddr_in);

	//server_addr = the tracker's own address info and client_addr = connecting peer's address 
   struct sockaddr_in server_addr, client_addr;   
	//creating the socket where sockid will be the integer identifier for this socket. 
   if ((sockid = socket(AF_INET,SOCK_STREAM,0)) < 0){//create socket connection oriented
	   printf("socket cannot be created \n"); exit(0); 
   }

  
	int yes=1;
	//Lose the pesky "Address already in use" error message
	if (setsockopt(sockid,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(int)) == -1) {
	perror("setsockopt");
	exit(1);
	}

    
   //socket created at this stage
   //now associate the socket with local port to allow listening incoming connections
   server_addr.sin_family = AF_INET;// assign address family
   server_addr.sin_port = htons(server_port);//change server port to NETWORK BYTE ORDER
   server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   
   //binding attaches the socket to the port specified 
   if (bind(sockid ,(struct sockaddr *) &server_addr,sizeof(server_addr)) ==-1){//bind and check error
	   printf("bind  failure\n"); exit(0); 
   }
    
   printf("Tracker SERVER READY TO LISTEN INCOMING REQUEST.... \n");
   if (listen(sockid,MAXREQUESTS) < 0){ //(parent) process listens at sockid and check error
	   printf(" Tracker  SERVER CANNOT LISTEN\n"); exit(0);
   }                                        
   
   while(1) { //accept  connection from every requester client
	//when a peer connects then create a brand new socket and store it inside of sock_child    
	if ((sock_child = accept(sockid ,(struct sockaddr *) &client_addr,&clilen))==-1){ /* Accept connection and create a socket descriptor for actual work */
		   printf("Tracker Cannot accept...\n"); exit(0); 
	   }
	   printf("Client connected\n"); 

	   //create a thread and have the peer_handler handle the thread! 
	   int *sock_ptr = malloc(sizeof(int)); 
	   *sock_ptr = sock_child; 
	   pthread_t thread; 
	   pthread_create(&thread,NULL,peer_handler,sock_ptr); 
	   pthread_detach(thread);
	   close(sock_child);
	        
	} //end of while loop 

	return 0; 
} //end of main 
     


//this function handles each connected peer 
void *peer_handler (void *arg) { //function for file transfer. child process will call this function     
    //start handiling client request	

	

	//the(int *) arg casts void* to int* and the second * dereferences the value 
	int sock_child = *(int *)arg; 
	int length;
	char read_msg[MAXLINE];
	char fname[MAXLINE];

	//it reads the message sent by the peer and stores it inside of length 
	length=read(sock_child,read_msg,MAXLINE);			
	read_msg[length]='\0';
	if((!strcmp(read_msg, "REQ LIST"))||(!strcmp(read_msg, "req list"))||(!strcmp(read_msg, "<REQ LIST>"))||(!strcmp(read_msg, "<REQ LIST>\n"))){//list command received
		handle_list_req(sock_child);// handle list request
		printf("list request handled.\n");
	}
	else if((strstr(read_msg,"get")!=NULL)||(strstr(read_msg,"GET")!=NULL)){// get command received
		xtrct_fname(read_msg, " ");// extract filename from the command		
		handle_get_req(sock_child, fname);		
	}
	else if((strstr(read_msg,"createtracker")!=NULL)||(strstr(read_msg,"Createtracker")!=NULL)||(strstr(read_msg,"CREATETRACKER")!=NULL)){// get command received
		tokenize_createmsg(read_msg);
		handle_createtracker_req(sock_child);
		
	}
	else if((strstr(read_msg,"updatetracker")!=NULL)||(strstr(read_msg,"Updatetracker")!=NULL)||(strstr(read_msg,"UPDATETRACKER")!=NULL)){// get command received
		tokenize_updatemsg(read_msg);
		handle_updatetracker_req(sock_child);		
	}

	free(arg); 
	return NULL; 
}//end client handler function


