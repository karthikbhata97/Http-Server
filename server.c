#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include "stimer.h"
#include "log.h"
#include "csuc_http.h"

static  char  	      path_root[PATH_MAX] = CURRENT_DIRECTORY;
static  int   	      port_number         = DEFAULT_PORT_NUMBER;
typedef void 	      (*strategy_t)(int);
static  int   	      buffer_max          = DEFAULT_BUFFER_SIZE;
static  int   	      worker_max          = DEFAULT_WORKER_SIZE;
static  sig_atomic_t  status_on           = True;
static  char  	      strategy_name[STRATEGY_MAX];
pthread_mutex_t buffer_lock_mtx          = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  buffer_not_full_cond      = PTHREAD_COND_INITIALIZER;
pthread_cond_t  buffer_not_empty_cond     = PTHREAD_COND_INITIALIZER;
timer_t total_uptime,requests_time;

const char* get_extension(const char *path)
{
	if(strstr(path,".HTML") || strstr(path,".html")) return "text/html";
	if(strstr(path,".JPEG") || strstr(path,".jpeg")) return "image/jpeg";
	if(strstr(path,".PNG" ) || strstr(path,".png" )) return "image/png";
	if(strstr(path,".TXT" ) || strstr(path,".txt" )) return "text";
	if(strstr(path,".JPG" ) || strstr(path,".jpg" )) return "image/jpg";
	if(strstr(path,".CSS" ) || strstr(path,".css" )) return "text/css";
	if(strstr(path,".JS"  ) || strstr(path,".js"  )) return "application/javascript";
	if(strstr(path,".XML" ) || strstr(path,".xml" )) return "application/xml";
	if(strstr(path,".MP3" ) || strstr(path,".mp3" )) return "audio/mpeg";
	if(strstr(path,".MPEG") || strstr(path,".mpeg")) return "video/mpeg";
	if(strstr(path,".MPG" ) || strstr(path,".mpg" )) return "video/mpeg";
	if(strstr(path,".MP4" ) || strstr(path,".mp4" )) return "video/mp4";
	if(strstr(path,".MOV" ) || strstr(path,".mov" )) return "video/quicktime";
	return "text/html";
}

static int next_request(int fd,http_request_t *request)
{
	char   command_line  [MAX_HEADER_LINE_LENGTH];
	char   method        [MAX_METHODS];
	char   payload_source[PATH_MAX];
	int    minor_version;
	int    major_version;
	char   head_name     [MAX_HEADER_LINE_LENGTH];
	char   head_value    [MAX_HEADER_VALUE_LENGTH];
	int    head_count = 0;
	FILE * fr;
	fr = fdopen(dup(fd),"r");	

	if(fr)
	{
		fgets(command_line,MAX_HEADER_LINE_LENGTH,fr);
		sscanf(command_line, "%s %s HTTP/%d.%d%*s",method,payload_source,&major_version,&minor_version);

		if(strcmp(method,"GET")==0)
		{
			request->method        = HTTP_METHOD_GET;
			trim_resource(payload_source);
			strcpy(request->uri,payload_source);
			request->major_version = major_version;
			request->minor_version = minor_version;
		}
		else
		{
			request->method        = HTTP_STATUS_NOT_IMPLEMENTED;
		}
	}
	while(head_count < MAX_HEADERS)
	{
		fgets(command_line,MAX_HEADER_LINE_LENGTH,fr);

		if(strstr(command_line,":"))
		{
			sscanf(command_line,"%s: %s",head_name,head_value);

			strcpy(request->headers[head_count].field_name,head_name);
			strcpy(request->headers[head_count++].field_value,head_value);    
		}
		else 
			break;
	}

	request->header_count=head_count;
	fclose(fr);
	increment_request();
	print_log(DEBUG,"\n Request number :%d",show_total_requests());
	return 1;
}

static int set_response_field_name_and_value(http_response_t *response,const char *name,const char *value)
{
	strcpy(response->headers[response->header_count].field_name,name);
	strcpy(response->headers[response->header_count++].field_value,value);
	return 1;
}

static int handle_error(http_status_t status,char * error_resource)
{
	if(status.code == HTTP_STATUS_LOOKUP[HTTP_STATUS_OK].code)	return 1;
	if(status.code == HTTP_STATUS_LOOKUP[HTTP_STATUS_NOT_FOUND].code)
	{
		strcpy(error_resource,path_root);
		strcat(error_resource,ERROR_NOT_FOUND_404);

		if(!check_file_exists(error_resource))
		{
			strcpy(error_resource,path_root);
			strcat(error_resource,ERROR_BAD_REQUEST_400);
		}
		if(!check_file_exists(error_resource))
		{
			strcpy(error_resource,DEFUALT_ERROR_NOT_FOUND_404);
		}
	}
	return 0;
}

static void set_signal_mask()
{
	static sigset_t   signal_mask;
	pthread_t  sig_thr_id;      
	sigemptyset(&signal_mask);
	sigaddset (&signal_mask, SIGINT);
	sigaddset (&signal_mask, SIGTERM);
	sigaddset (&signal_mask, SIGUSR1);
	sigaddset (&signal_mask, SIGUSR2);
	pthread_sigmask (SIG_BLOCK, &signal_mask, NULL);
}

static http_status_t check_response_status(const int status,const char * path)
{
	if(status == HTTP_STATUS_NOT_IMPLEMENTED) return HTTP_STATUS_LOOKUP[status];
	if(!check_file_exists(path))              return HTTP_STATUS_LOOKUP[HTTP_STATUS_NOT_FOUND];
	return HTTP_STATUS_LOOKUP[HTTP_STATUS_OK];
}

static int build_response(const http_request_t *request,http_response_t *response)
{
	char buffer[MAX_HEADER_VALUE_LENGTH];
	int head_count=0;
	time_t now=0;
	struct tm *t;
	strcat(response->resource_path,request->uri);
	set_index(response->resource_path);	

	response->status        = check_response_status(request->method,response->resource_path);
	handle_error(response->status, response->resource_path);

	response->major_version = request->major_version;
	response->minor_version = request->minor_version;

	now = time(NULL);
	t = gmtime(&now);
	strftime(buffer,30,"%a, %d %b %Y %H:%M:%S %Z",t);

	set_response_field_name_and_value(response,"Date",buffer);
	set_response_field_name_and_value(response,"Server","CSUC HTTP");
	set_response_field_name_and_value(response,"Content-Type",get_extension(response->resource_path));
	sprintf(buffer,"%d",file_size(response->resource_path));
	add_to_total_size(atoi(buffer));
	set_response_field_name_and_value(response,"Content-Length",buffer);

	return 1;
}

static int send_response(int fd,const http_response_t *response)
{
	time_t now;
	struct tm t;
	FILE   *fr;
	char   buf[MAX_HEADER_VALUE_LENGTH];
	FILE   *fp = fdopen(dup(fd),"w");
	size_t size;
	int    head_no;
	int    ch;	
	fprintf(fp,"HTTP/%d.%d %d %s\r\n",response->major_version,response->minor_version,response->status.code,response->status.reason);
	for(head_no = 0; head_no<response->header_count; head_no++)
	{
		fprintf(fp,"%s: %s\r\n",response->headers[head_no].field_name,response->headers[head_no].field_value);
	}
	fprintf(fp,"\n");
	//sleep(1);                             //test parallel implementation 
	fr=fopen(response->resource_path,"r");  //print payload 
	if(fr)
	{
		while((ch=getc(fr))!=EOF)  fprintf(fp,"%c",ch);
		fclose(fr);
	}
	fclose(fp);
	return 1;
}    

static int clear_responses(http_response_t *response)//clearing responses avoids possiblity of duplicate headers err
{
	int head_no;
	for(head_no =0; head_no<response->header_count; head_no++)
	{
		strcpy(response->headers[head_no].field_name,"");
		strcpy(response->headers[head_no].field_value,"");
	}
	response->header_count = 0;
	return 1;
}

static void manage_single_request(int peer_sfd)
{
	s_start(&requests_time);
	http_request_t  *request  = (http_request_t*)malloc(sizeof(http_request_t));	
	http_response_t *response = (http_response_t*)malloc(sizeof(http_response_t));	
	strcpy(response->resource_path,path_root);

	next_request(peer_sfd, request);
	build_response(request, response);
	send_response(peer_sfd, response);

	clear_responses(response);
	free(request);
	free(response);	
	s_stop(&requests_time);
	get_time_difference(&requests_time);
}

static void perform_process_operation(int sfd)
{
	print_log(DEBUG,"PERFORMING USING FORK");
	while(status_on)                                                        
	{
		int peer_sfd = accept(sfd,NULL,NULL);                           

		if(peer_sfd == -1)
		{
			print_log(WARNING,"\nAccept failed may be because of Interrupt");
			continue;
		}
		pid_t cid = fork();
		if(cid < 0)
		{
			print_log(ERROR,"Fork Error");
			exit(0);
		}
		else if(cid == 0)
		{	
			manage_single_request(peer_sfd);
			exit(0);
		}
		else if(cid > 0)
		{
			while(waitpid(-1,NULL,WNOHANG)>0);  // Kill zombies for better tomorrow
		}
		close(peer_sfd);
	}
}
static void perform_serially(int sfd)
{
	print_log(DEBUG,"\nPERFORMING SERIALLY");
	while(status_on)
	{
		int peer_sfd = accept(sfd,NULL,NULL);                           

		if(peer_sfd == -1)
		{	
			print_log(WARNING,"\nACCEPT FAILED MAY BE BECAUSE OF INTERRUPT");
			continue;
		}
		manage_single_request(peer_sfd);
		close(peer_sfd);
	}
}

static void producer_operation(int sfd)
{
	while(status_on)
	{	
		int peer_sfd = accept(sfd,NULL,NULL);                           
		if(peer_sfd<0) 
		{
			print_log(WARNING,"\nACCEPT FAILED MAY BE BECAUSE OF INTERRUPT");
			continue;
		}
		if(pthread_mutex_lock(&buffer_lock_mtx)!=0)
			print_log(WARNING,"\nthere is error in lock prod");
		while(is_buffer_full()) 
		{
			if(pthread_cond_wait(&buffer_not_full_cond,&buffer_lock_mtx)!=0)
				print_log(WARNING,"\nthere is error in wait prod");
		}

		buffer_add(peer_sfd);				

		if(pthread_cond_broadcast(&buffer_not_empty_cond)!=0)
			print_log(WARNING,"\nthere is error in broadcast");
		if(pthread_mutex_unlock(&buffer_lock_mtx)!=0)
			print_log(WARNING,"\nthere is erorr in unlock");
	}
	destroy_buffer();
}

static void* consumer_operation()
{
	set_signal_mask();
	while(status_on)
	{	
	 	if(pthread_mutex_lock(&buffer_lock_mtx)!=0)
			print_log(WARNING,"\nthere is error in lock cons");
		
		while(is_buffer_empty())
		{	
			if(pthread_cond_wait(&buffer_not_empty_cond,&buffer_lock_mtx)!=0)
				print_log(WARNING,"\nthere is error in wait cons");
		}	

		int peer_sfd = buffer_get();

		if(peer_sfd < 0) 
		{
			print_log(WARNING,"\nRECIEVED FAILED ACCEPT SOCKET");
			pthread_mutex_unlock(&buffer_lock_mtx)!=0;
			continue;
		}

		if(pthread_cond_broadcast(&buffer_not_full_cond)!=0)
			print_log(WARNING,"\nthere is error in broadcast cons");
		if(pthread_mutex_unlock(&buffer_lock_mtx)!=0)
			print_log(WARNING,"\nthere is error in unlock cons");

		manage_single_request(peer_sfd);
		close(peer_sfd);
	}
	pthread_exit(pthread_self);
}

static void perform_thread_pool_operation(int sfd)
{	
	print_log(DEBUG,"PERFORMING THREAD POOL");
	int worker_no = 0;
	pthread_t tid;
	init_buffer(buffer_max);
	
	for(worker_no = 0 ; worker_no < worker_max; worker_no++)
	{
		pthread_create(&tid,NULL,consumer_operation,NULL);
	}
	
	producer_operation(sfd);	
}

static void* manage_request_response_per_thread(void *peer_sfd)
{
	set_signal_mask();
	manage_single_request(*(int*)peer_sfd);
	close(*(int*)peer_sfd);
	free((int*)peer_sfd);
	pthread_exit(pthread_self);
}

static void perform_thread_operation(int sfd)
{
     	print_log(DEBUG,"PERFORMING USING THREADS");
	pthread_t tid;
	while(status_on)                                                        
	{
		int *peer_sfd = malloc(sizeof(int));
		*peer_sfd = accept(sfd,NULL,NULL);                           
		if(*peer_sfd == -1)
		{
			print_log(WARNING,"\nACCEPT FAILED MAY BE BECAUSE OF INTERRUPT");
			continue;
		}
		pthread_create(&tid,NULL,manage_request_response_per_thread,peer_sfd);
		pthread_detach(tid);
	}
}

void display_information(int sig)
{

	print_log(INFO,"\n-----------------------------SERVER iNFO-----------------------------");
	print_log(ERROR,"\nCURRENT LOG LEVELS              : ERROR");
	print_log(WARNING,",WARNING");
	print_log(INFO,",INFO");
	print_log(DEBUG,",DEBUG");
	print_log(INFO,"\nDocument Root                    : %s",path_root);
	print_log(INFO,"\nPort No                          : %d",port_number);
	print_log(INFO,"\nResponse Strategy                : %s",strategy_name);
	
	if(strstr(strategy_name,"Fork")) 
	{	
		
		print_log(INFO,"\n---------------------------------------------------------------------\n");
		return;
	}
	if(strcmp(strategy_name,"Thread Pool")==0)
	{
		print_log(INFO,"\nThread Pool Size                 : %d",worker_max);
		print_log(INFO,"\nWorker Size        	         : %d",buffer_max);
	}

	print_log(INFO,"\nTotal Requests handled           : %d",show_total_requests());
	print_log(INFO,"\nTotal amount of data transferred : %d bytes",show_total_size());
	s_stop(&total_uptime);
	get_time_difference(&total_uptime);
	print_log(INFO,"\nTotal uptime                     : %s",show_time_difference(&total_uptime));
	print_log(INFO,"\nTotal time spent serving requets : %s",show_total_time_difference(&requests_time));

	print_log(INFO,"\nAvg time spent serving requests  : %s",show_average_time(&requests_time,show_total_requests()));
	print_log(INFO,"\n---------------------------------------------------------------------\n");
}

void graceful_shutdown(int sig)
{
	status_on = False;
}

void change_log(int sig)
{
	set_next_log();
}

void handle_signals()
{
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags   = 0;

	sa.sa_handler = graceful_shutdown;
	if (sigaction(SIGINT,  &sa, NULL) == -1)  exit(0);
	if (sigaction(SIGTERM, &sa, NULL) == -1)  exit(0);
	sa.sa_handler = display_information;
	if (sigaction(SIGUSR1, &sa, NULL) == -1)  exit(0);
	sa.sa_handler = change_log;
	if (sigaction(SIGUSR2, &sa, NULL) == -1)  exit(0);
}

int initialize_server()
{
	struct sockaddr_in myaddr;
	int    sfd; 
	int    optval = 1;

	sfd = socket(AF_INET,SOCK_STREAM,0);    //creating socket

	if(sfd == -1)
	{
		print_log(ERROR,"socket");
		exit(0);
	}

	if(setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
		print_log(WARNING,"\nsetsockopt");

	memset(&myaddr,0,sizeof(myaddr));
	myaddr.sin_family      = AF_INET;
	myaddr.sin_port        = htons(port_number);
	myaddr.sin_addr.s_addr = INADDR_ANY;

	if(bind(sfd, (struct sockaddr*) &myaddr, sizeof(myaddr)) == -1)  
	{	
		print_log(ERROR,"PORT NO NOT FOUND GLOBAL ERROR STATUS IS:");
		exit(0);
	}
	if(listen(sfd,BACKLOG)==-1)                                     
		print_log(WARNING,"\nLISTEN FAILED");             

	handle_signals();
	return sfd;
}

strategy_t configure_server(int argc,char *argv[])
{
	int option,option_count=0;
	strategy_t operation;
	s_start(&total_uptime);

	while((option = getopt(argc,argv,"p:d:w:q:ftv:"))!=-1)
	{
		switch (option)
		{
			case 'p':
				port_number = atoi(optarg);
				break;
			case 'd':
				strcpy(path_root,optarg);
				break;
			case 't': 
				strcpy(strategy_name,"Threads");
				operation = &perform_thread_operation;
				option_count++;
				break;
			case 'f':
				strcpy(strategy_name,"Fork(Using processes)");
				operation = &perform_process_operation;
				option_count++;
				break;
			case 'w':
				strcpy(strategy_name,"Thread Pool");
				worker_max = atoi(optarg);
				operation = &perform_thread_pool_operation;
				option_count++;
				break;
			case 'q':
				buffer_max = atoi(optarg);
				break;
			case 'v':
				set_log(optarg);
				break;
			default:
				print_log(ERROR,"Please enter the right arguments\n");
				break;
		}
	}
	if(option_count > 1)
	{
		print_log(ERROR,"\nDon't pass arguments to use more than one strategy.\n");
		exit(0);
	}
	
	if(!check_folder_exists(path_root)) exit(0);
	
	if(option_count ==0) 
	{	
		operation = &perform_serially;
		strcpy(strategy_name,"Serial Operation");
	}
	return operation;
}
int main(int argc,char *argv[])
{	
	strategy_t server_operation;
	int sfd;
	server_operation = (strategy_t)configure_server(argc,argv);		
	sfd              = initialize_server();
	printf("Running server with pid %d\n", getpid());
	server_operation(sfd);  				//start server
	if(close(sfd)==-1)					//close server
		print_log(WARNING,"\nError while closing");

	return 0;
}
