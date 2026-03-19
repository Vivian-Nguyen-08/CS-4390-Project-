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
#include <dirent.h> 
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h> 

//create a struct to make it easier to pass values when doing createtracker 
typedef struct {
    char file_name[256];
    char file_size[256];
    char description[256];
    char md5[64];
	char ip_address[256];
	char port_number[256];
	bool success; 
} fileEntry; 



//global variables 
#define MAXLINE 512
#define MAXREQUESTS 20

//function declarations 
char shared_directory[256];  
char temp[512]; 
void *peer_handler(void *arg);
void handle_list_req(int sock_child); 
void handle_get_req(int sock_child, char *fname);
void xtrct_fname(char *msg, char *delim,char *fname); 
fileEntry tokenize_createmsg(char *msg); 
void handle_createtracker_req(int sock_child,fileEntry new_entry); 
void tokenize_updatemsg(char *msg){}
void handle_updatetracker_req(int sock_child){}


int main(){
	//declaring local variables 
	int sockid; 
    int sock_child;
    int server_port;
    socklen_t clilen;


	//will read the sconfig file and get the port number and shared directory 
	FILE *fptr; 
	fptr = fopen("sconfig","r");
	if(fptr == NULL){
    	printf("Cannot open sconfig\n");
    	exit(0);
	}

	char port_number[256]; 
	fgets(port_number,sizeof(port_number),fptr); 
	fgets(shared_directory,sizeof(shared_directory),fptr); 
	fclose(fptr); 

	port_number[strcspn(port_number, "\n")] = '\0';
	shared_directory[strcspn(shared_directory, "\n")] = '\0';

	server_port = atoi(port_number); 

	//creates the shared directory if it isn't already made 
	struct stat st = {0};
	if (stat(shared_directory, &st) == -1) {
		mkdir(shared_directory, 0700);
	}
   


	clilen = sizeof(struct sockaddr_in);

	//server_addr = the tracker's own address info and client_addr = connecting peer's address 
   struct sockaddr_in server_addr, client_addr;   
	//creating the socket where sockid will be the integer identifier for this socket. 
   if ((sockid = socket(AF_INET,SOCK_STREAM,0)) < 0){//create socket connection oriented
	   printf("socket cannot be created \n"); exit(0); 
   }

  
	int yes=1;
	//lose the pesky "Address already in use" error message
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
	read_msg[strcspn(read_msg, "\r\n")] = '\0';

	if(length <= 0){
    printf("read failed or client disconnected\n");
    close(sock_child);
    free(arg);
    return NULL;
}

	if((!strcmp(read_msg, "REQ LIST"))||(!strcmp(read_msg, "req list"))||(!strcmp(read_msg, "<REQ LIST>"))||(!strcmp(read_msg, "<REQ LIST>\n"))){//list command received
		handle_list_req(sock_child);// handle list request
		printf("list request handled.\n");
	}
	else if((strstr(read_msg,"get")!=NULL)||(strstr(read_msg,"GET")!=NULL)){// get command received
		xtrct_fname(read_msg, " ",fname);// extract filename from the command		
		handle_get_req(sock_child, fname);		
	}
	else if((strstr(read_msg,"createtracker")!=NULL)||(strstr(read_msg,"Createtracker")!=NULL)||(strstr(read_msg,"CREATETRACKER")!=NULL)){// get command received
		fileEntry new_entry = tokenize_createmsg(read_msg);
		handle_createtracker_req(sock_child,new_entry);
		
	}
	else if((strstr(read_msg,"updatetracker")!=NULL)||(strstr(read_msg,"Updatetracker")!=NULL)||(strstr(read_msg,"UPDATETRACKER")!=NULL)){// get command received
		tokenize_updatemsg(read_msg);
		handle_updatetracker_req(sock_child);		
	}

	free(arg); 
	close(sock_child);
	return NULL; 
}//end client handler function



//LIST – This command is sent by a connected peer to the tracker server to send over to the requesting peer the list of  (tracker) files in the shared directory at the server. 
void handle_list_req(int sock_child)
{
	struct dirent *de;
	int fileCounter = 1; 
	char *msg = malloc(1000000);
	msg[0] = '\0';

	//open the shared directory 
	DIR *dr = opendir(shared_directory); 

	if(dr == NULL)  {
		printf("Could not open current directory"); 
	} 

	//go through the directory and get every file and print out each file information 
	while((de = readdir(dr)) != NULL)
	{
		if(strstr(de->d_name, ".track") == NULL){
    		continue;
		}
		//formats the filepath to check it out 
		char filepath[512]; 
		sprintf(filepath, "%s/%s",shared_directory, de->d_name); 

		char filename[256],filesize[256],md5[256]; 
		char line[512]; 

		//opens up the file 
		FILE *fptr; 
		fptr = fopen(filepath,"r");
		if(fptr == NULL){
			printf("Cannot open sconfig\n");
			exit(0);
		}

	
	//get filename 
	fgets(line,sizeof(line),fptr); 
	sscanf(line, "%*[^:]: %s", filename); 

	//get filesize 
	fgets(line,sizeof(line),fptr); 
	sscanf(line, "%*[^:]: %s", filesize);

	//skip description 
	fgets(line,sizeof(line),fptr); 

	//get md5 
	fgets(line,sizeof(line),fptr); 
	sscanf(line, "%*[^:]: %s", md5);

	fclose(fptr); 
	
	//formats what will be showing the peer for each file 
	sprintf(temp, "<%d %s %s %s>\n", fileCounter, filename, filesize, md5);
	strcat(msg,temp); 
	fileCounter++; 

	}

	//send the message to the peer
	sprintf(temp,"<REP LIST %d>\n",fileCounter -1);
	send(sock_child, temp, strlen(temp), 0);
	send(sock_child, msg, strlen(msg), 0);
	send(sock_child, "<REP LIST END>\n", 15, 0);


	closedir(dr); 
	free(msg);
	return; 

}

 //GET: sends the tracker file being requested.
void handle_get_req(int sock_child, char *fname){

	//open the file and extract all of the data we need 
	char filepath[512]; 
	sprintf(filepath, "%s/%s",shared_directory, fname); 

	char md5[256]; 
	char line[512]; 

	//open the file the peer is requesting for 
	FILE *fptr; 
	fptr = fopen(filepath,"r");
	if(fptr == NULL){
		//printf("GET: file not found: %s\n", filepath);
    	send(sock_child, "<GET invalid>\n", 14, 0);
    	return;
	}

	//send the get begin 
	send(sock_child, "<REP GET BEGIN>\n", 16, 0);
	
	//send the rest of the file content 
	//send entire file content line by line
	while(fgets(line, sizeof(line), fptr) != NULL){
		if(line[0] == '#'){
        continue;
    }
    send(sock_child, line, strlen(line), 0);
    //grab md5 from line 4
    if(strncmp(line, "MD5:", 4) == 0){
        sscanf(line, "%*[^:]: %s", md5);
    }
}
	//send the ending 
	sprintf(temp,"\n<REP GET END %s>\n",md5);
	send(sock_child, temp, strlen(temp), 0);

	fclose(fptr); 


	return; 
}

void handle_createtracker_req(int sock_child,fileEntry new_entry)
{
	//checking to see all of the values needed to make a file are there and if they aren't send fail 
	if(new_entry.success == false) {
		send(sock_child, "<createtracker fail>\n", 21, 0);
		return;
	}
	
	//format the filepath 
	char filepath[512]; 
	sprintf(filepath, "%s/%s.track",shared_directory, new_entry.file_name); 


	//open the file to see if the file already exists and if it does then tell peer the file already exists 
	FILE *fptr; 
	fptr = fopen(filepath,"r");
	if(fptr != NULL){
		fclose(fptr);
    	send(sock_child, "<createtracker ferr>\n", 21, 0);
    	return;
	}

	//create the file then write into it 
	fptr = fopen(filepath, "w");
	if(fptr == NULL){
		// couldn't create the file for some reason
		send(sock_child, "<createtracker fail>\n", 21, 0);
		return;
	}

	//write contents into the file 
	fprintf(fptr, "Filename: %s\n", new_entry.file_name);
	fprintf(fptr, "Filesize: %s\n", new_entry.file_size);
	fprintf(fptr, "Description: %s\n", new_entry.description);
	fprintf(fptr, "MD5: %s\n", new_entry.md5);
	fprintf(fptr, "#list of peers follows next\n");
	fprintf(fptr, "%s:%s:0:%s:%ld\n", new_entry.ip_address, new_entry.port_number, new_entry.file_size, time(NULL));

	fclose(fptr);

	//send success
	send(sock_child, "<createtracker succ>\n", 21, 0);

	return; 
}



void xtrct_fname(char *msg, char *delim,char *fname){
	char *myPtr = strstr(msg,delim); 

	if(myPtr != NULL) {
	strcpy(fname, myPtr+1);		
	fname[strcspn(fname, ">\n\r")] = '\0';
	}


}


fileEntry tokenize_createmsg(char *msg){
	fileEntry new_entry; 
	new_entry.success = true; 

	//remove trailing >
	msg[strcspn(msg, ">")] = '\0';
	
	//start the token 
	char *token = strtok(msg, " "); 

	//get filename 
	token = strtok(NULL, " "); 
	if (token != NULL) 
    	strcpy(new_entry.file_name, token);
	else 
		new_entry.success = false; 

	//get filesize 
	token = strtok(NULL, " "); 
	if (token != NULL) 
    	strcpy(new_entry.file_size, token);
	else 
		new_entry.success = false; 

	//get description 
	token = strtok(NULL, " "); 
	if (token != NULL) 
    	strcpy(new_entry.description, token);
	else 
		new_entry.success = false; 
	
	//get md5 
	token = strtok(NULL, " "); 
	if (token != NULL) 
    	strcpy(new_entry.md5, token); 
	else 
		new_entry.success = false; 
	
	//get ip_address 
	token = strtok(NULL, " "); 
	if (token != NULL) 
    	strcpy(new_entry.ip_address, token);
	else 
		new_entry.success = false; 
	
	//get port_number 
	token = strtok(NULL, " "); 
	if (token != NULL) 
    	strcpy(new_entry.port_number, token);
	else 
		new_entry.success = false; 

	
	return new_entry; 


}

