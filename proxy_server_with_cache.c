#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_SIZE 200*(1<<20)
#define MAX_ELEMENT_SIZE 10*(1<<20)

// CPU Scheduling constants
#define TIME_QUANTUM 100000  // 100ms in microseconds
#define MAX_PRIORITY 5
#define MIN_PRIORITY 1

// Memory Pool constants
#define MEMORY_POOL_SIZE (50 * 1024 * 1024)  // 50MB
#define BLOCK_HEADER_SIZE sizeof(memory_block)
#define MIN_BLOCK_SIZE 64

// Memory block structure for custom allocator
typedef struct memory_block {
    size_t size;
    int is_free;
    struct memory_block* next;
    struct memory_block* prev;
} memory_block;

// Thread control structure for scheduling
typedef struct thread_control {
    pthread_t thread_id;
    int thread_index;
    int priority;
    int cpu_time_used;
    int time_quantum_remaining;
    int status; // 0=ready, 1=running, 2=blocked, 3=terminated
    time_t creation_time;
    time_t last_scheduled;
    struct thread_control* next;
} thread_control;

// Cache element structure
typedef struct cache_element cache_element;
struct cache_element{
    char* data;
    int len;
    char* url;
    time_t lru_time_track;
    cache_element* next;
};

// Function declarations
cache_element* find(char* url);
int add_cache_element(char* data,int size,char* url);
void remove_cache_element();

// Memory management functions
void* custom_malloc(size_t size);
void custom_free(void* ptr);
void init_memory_pool();
memory_block* find_free_block(size_t size);
void split_block(memory_block* block, size_t size);
void merge_blocks();

// CPU scheduling functions
void init_scheduler();
void add_thread_to_scheduler(pthread_t tid, int index);
void remove_thread_from_scheduler(pthread_t tid);
void* scheduler_thread(void* arg);
void round_robin_schedule();
thread_control* get_next_thread();
void update_thread_priority(thread_control* tc);

// Global variables
int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t seamaphore;
pthread_mutex_t lock;
pthread_mutex_t memory_lock;
pthread_mutex_t scheduler_lock;

cache_element* head;
int cache_size;

// Memory pool globals
static char memory_pool[MEMORY_POOL_SIZE];
static memory_block* memory_head = NULL;
static int memory_initialized = 0;

// Scheduler globals
thread_control* scheduler_head = NULL;
thread_control* current_running_thread = NULL;
pthread_t scheduler_tid;
int scheduler_running = 1;

// Timer for round robin scheduling
timer_t timer_id;

// Custom Memory Allocator Implementation
void init_memory_pool() {
    if (memory_initialized) return;
    
    pthread_mutex_lock(&memory_lock);
    
    memory_head = (memory_block*)memory_pool;
    memory_head->size = MEMORY_POOL_SIZE - BLOCK_HEADER_SIZE;
    memory_head->is_free = 1;
    memory_head->next = NULL;
    memory_head->prev = NULL;
    
    memory_initialized = 1;
    pthread_mutex_unlock(&memory_lock);
}

memory_block* find_free_block(size_t size) {
    memory_block* current = memory_head;
    
    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void split_block(memory_block* block, size_t size) {
    if (block->size <= size + BLOCK_HEADER_SIZE + MIN_BLOCK_SIZE) {
        return; // Don't split if remaining would be too small
    }
    
    memory_block* new_block = (memory_block*)((char*)block + BLOCK_HEADER_SIZE + size);
    new_block->size = block->size - size - BLOCK_HEADER_SIZE;
    new_block->is_free = 1;
    new_block->next = block->next;
    new_block->prev = block;
    
    if (block->next) {
        block->next->prev = new_block;
    }
    
    block->next = new_block;
    block->size = size;
}

void merge_blocks() {
    memory_block* current = memory_head;
    
    while (current && current->next) {
        if (current->is_free && current->next->is_free) {
            current->size += current->next->size + BLOCK_HEADER_SIZE;
            memory_block* temp = current->next;
            current->next = temp->next;
            if (temp->next) {
                temp->next->prev = current;
            }
        } else {
            current = current->next;
        }
    }
}

void* custom_malloc(size_t size) {
    if (size == 0) return NULL;
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    pthread_mutex_lock(&memory_lock);
    
    if (!memory_initialized) {
        init_memory_pool();
    }
    
    memory_block* block = find_free_block(size);
    
    if (!block) {
        pthread_mutex_unlock(&memory_lock);
        printf("Custom malloc: Out of memory\n");
        printf("malloc(%zu)\n", size);  // ADD THIS LINE
        return malloc(size); // Fallback to system malloc
    }
    
    split_block(block, size);
    block->is_free = 0;
    
    pthread_mutex_unlock(&memory_lock);
    printf("malloc(%zu)\n", size);  // ADD THIS LINE
    return (char*)block + BLOCK_HEADER_SIZE;
}

void custom_free(void* ptr) {
    if (!ptr) return;
    
    pthread_mutex_lock(&memory_lock);
    
    memory_block* block = (memory_block*)((char*)ptr - BLOCK_HEADER_SIZE);
    
    // Verify this is within our memory pool
    if ((char*)block < memory_pool || (char*)block >= memory_pool + MEMORY_POOL_SIZE) {
        pthread_mutex_unlock(&memory_lock);
        free(ptr); // Fallback to system free
        return;
    }
    
    block->is_free = 1;
    printf("Block freed\n");  // ADD THIS LINE
    merge_blocks();
    
    pthread_mutex_unlock(&memory_lock);
}

// CPU Scheduler Implementation
void init_scheduler() {
    pthread_mutex_init(&scheduler_lock, NULL);
    scheduler_head = NULL;
    current_running_thread = NULL;
    
    // Create scheduler thread
    pthread_create(&scheduler_tid, NULL, scheduler_thread, NULL);
    
    printf("CPU Scheduler initialized with Round Robin algorithm\n");
}

void add_thread_to_scheduler(pthread_t tid, int index) {
    pthread_mutex_lock(&scheduler_lock);
    
    thread_control* new_tc = (thread_control*)custom_malloc(sizeof(thread_control));
    new_tc->thread_id = tid;
    new_tc->thread_index = index;
    new_tc->priority = MIN_PRIORITY + (index % MAX_PRIORITY);
    new_tc->cpu_time_used = 0;
    new_tc->time_quantum_remaining = TIME_QUANTUM;
    new_tc->status = 0; // ready
    new_tc->creation_time = time(NULL);
    new_tc->last_scheduled = 0;
    new_tc->next = scheduler_head;
    
    scheduler_head = new_tc;
    
    pthread_mutex_unlock(&scheduler_lock);
    
    printf("Thread %d added to scheduler with priority %d\n", index, new_tc->priority);
}

void remove_thread_from_scheduler(pthread_t tid) {
    pthread_mutex_lock(&scheduler_lock);
    
    thread_control* current = scheduler_head;
    thread_control* prev = NULL;
    
    while (current) {
        if (pthread_equal(current->thread_id, tid)) {
            if (prev) {
                prev->next = current->next;
            } else {
                scheduler_head = current->next;
            }
            
            if (current_running_thread == current) {
                current_running_thread = NULL;
            }
            
            printf("Thread %d removed from scheduler\n", current->thread_index);
            custom_free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&scheduler_lock);
}

thread_control* get_next_thread() {
    thread_control* current = scheduler_head;
    thread_control* best_candidate = NULL;
    time_t current_time = time(NULL);
    
    while (current) {
        if (current->status == 0) { // ready state
            if (!best_candidate || 
                current->priority > best_candidate->priority ||
                (current->priority == best_candidate->priority && 
                 current->last_scheduled < best_candidate->last_scheduled)) {
                best_candidate = current;
            }
        }
        current = current->next;
    }
    
    return best_candidate;
}

void update_thread_priority(thread_control* tc) {
    // Dynamic priority adjustment based on CPU usage
    if (tc->cpu_time_used > TIME_QUANTUM * 3) {
        if (tc->priority > MIN_PRIORITY) {
            tc->priority--;
            printf("Thread %d priority decreased to %d\n", tc->thread_index, tc->priority);  // ADD THIS
        }
    } else if (tc->cpu_time_used < TIME_QUANTUM / 2) {
        if (tc->priority < MAX_PRIORITY) {
            tc->priority++;
            printf("Thread %d priority increased to %d\n", tc->thread_index, tc->priority);  // ADD THIS
        }
    }
}

void* scheduler_thread(void* arg) {
    while (scheduler_running) {
        usleep(TIME_QUANTUM / 10); // Check every 10ms
        
        pthread_mutex_lock(&scheduler_lock);
        round_robin_schedule();
        pthread_mutex_unlock(&scheduler_lock);
    }
    return NULL;
}

void round_robin_schedule() {
    time_t current_time = time(NULL);
    
    // Check if current thread needs to be preempted
    if (current_running_thread) {
        current_running_thread->time_quantum_remaining -= TIME_QUANTUM / 10;
        
        if (current_running_thread->time_quantum_remaining <= 0) {
            current_running_thread->status = 0; // back to ready
            current_running_thread->time_quantum_remaining = TIME_QUANTUM;
            update_thread_priority(current_running_thread);
            current_running_thread = NULL;
        }
    }
    
    // Select next thread to run
    if (!current_running_thread) {
        thread_control* next_thread = get_next_thread();
        
        if (next_thread) {
            next_thread->status = 1; // running
            next_thread->last_scheduled = current_time;
            current_running_thread = next_thread;
        }
    }
}

// Original proxy server functions with memory management integration

int sendErrorMessage(int socket, int status_code)
{
    char *str = (char*)custom_malloc(1024);
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code)
    {
        case 400: snprintf(str, 1024, "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
                  printf("400 Bad Request\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 403: snprintf(str, 1024, "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
                  printf("403 Forbidden\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 404: snprintf(str, 1024, "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
                  printf("404 Not Found\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 500: snprintf(str, 1024, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
                  send(socket, str, strlen(str), 0);
                  break;

        case 501: snprintf(str, 1024, "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
                  printf("501 Not Implemented\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 505: snprintf(str, 1024, "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
                  printf("505 HTTP Version Not Supported\n");
                  send(socket, str, strlen(str), 0);
                  break;

        default:  
                  custom_free(str);
                  return -1;
    }
    custom_free(str);
    return 1;
}

int connectRemoteServer(char* host_addr, int port_num)
{
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

    if( remoteSocket < 0)
    {
        printf("Error in Creating Socket.\n");
        return -1;
    }
    
    struct hostent *host = gethostbyname(host_addr);    
    if(host == NULL)
    {
        fprintf(stderr, "No such host exists.\n");    
        return -1;
    }

    struct sockaddr_in server_addr;

    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);

    bcopy((char *)host->h_addr,(char *)&server_addr.sin_addr.s_addr,host->h_length);

    if( connect(remoteSocket, (struct sockaddr*)&server_addr, (socklen_t)sizeof(server_addr)) < 0 )
    {
        fprintf(stderr, "Error in connecting !\n"); 
        return -1;
    }
    return remoteSocket;
}

int handle_request(int clientSocket, ParsedRequest *request, char *tempReq)
{
    char *buf = (char*)custom_malloc(sizeof(char)*MAX_BYTES);
    strcpy(buf, "GET ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);

    if (ParsedHeader_set(request, "Connection", "close") < 0){
        printf("set header key not work\n");
    }

    if(ParsedHeader_get(request, "Host") == NULL)
    {
        if(ParsedHeader_set(request, "Host", request->host) < 0){
            printf("Set \"Host\" header key not working\n");
        }
    }

    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0) {
        printf("unparse failed\n");
    }

    int server_port = 80;
    if(request->port != NULL)
        server_port = atoi(request->port);

    int remoteSocketID = connectRemoteServer(request->host, server_port);

    if(remoteSocketID < 0) {
        custom_free(buf);
        return -1;
    }

    int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);

    bzero(buf, MAX_BYTES);

    bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    char *temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES);
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    while(bytes_send > 0)
    {
        bytes_send = send(clientSocket, buf, bytes_send, 0);
        
        for(int i=0;i<bytes_send/sizeof(char);i++){
            temp_buffer[temp_buffer_index] = buf[i];
            temp_buffer_index++;
        }
        temp_buffer_size += MAX_BYTES;
        temp_buffer=(char*)realloc(temp_buffer,temp_buffer_size);

        if(bytes_send < 0)
        {
            perror("Error in sending data to client socket.\n");
            break;
        }
        bzero(buf, MAX_BYTES);

        bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
    } 
    temp_buffer[temp_buffer_index]='\0';
    custom_free(buf);
    add_cache_element(temp_buffer, strlen(temp_buffer), tempReq);
    printf("Done\n");
    free(temp_buffer); // This was allocated with realloc, so use free
    
    close(remoteSocketID);
    return 0;
}

int checkHTTPversion(char *msg)
{
    int version = -1;

    if(strncmp(msg, "HTTP/1.1", 8) == 0)
    {
        version = 1;
    }
    else if(strncmp(msg, "HTTP/1.0", 8) == 0)            
    {
        version = 1;
    }
    else
        version = -1;

    return version;
}

void* thread_fn(void* socketNew)
{
    sem_wait(&seamaphore); 
    int p;
    sem_getvalue(&seamaphore,&p);
    printf("semaphore value:%d\n",p);
    int* t= (int*)(socketNew);
    int socket=*t;
    int bytes_send_client,len;

    // Add this thread to scheduler
    add_thread_to_scheduler(pthread_self(), socket);
    
    char *buffer = (char*)custom_malloc(MAX_BYTES*sizeof(char));
    
    bzero(buffer, MAX_BYTES);
    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);
    
    while(bytes_send_client > 0)
    {
        len = strlen(buffer);
        if(strstr(buffer, "\r\n\r\n") == NULL)
        {    
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        }
        else{
            break;
        }
        
        // Yield CPU periodically for scheduling
        usleep(1000); // 1ms
    }
    
    char *tempReq = (char*)custom_malloc((strlen(buffer)+1)*sizeof(char));
    for (int i = 0; i < strlen(buffer); i++)
    {
        tempReq[i] = buffer[i];
    }
    
    struct cache_element* temp = find(tempReq);

    if( temp != NULL){
        int size=temp->len/sizeof(char);
        int pos=0;
        char *response = (char*)custom_malloc(MAX_BYTES);
        while(pos<size){
            bzero(response,MAX_BYTES);
            for(int i=0;i<MAX_BYTES && pos<size;i++){
                response[i]=temp->data[pos];
                pos++;
            }
            send(socket,response,MAX_BYTES,0);
        }
        printf("Data retrieved from the Cache\n\n");
        custom_free(response);
    }
    else if(bytes_send_client > 0)
    {
        len = strlen(buffer); 
        ParsedRequest* request = ParsedRequest_create();
        
        if (ParsedRequest_parse(request, buffer, len) < 0) 
        {
               printf("Parsing failed\n");
        }
        else
        {    
            bzero(buffer, MAX_BYTES);
            if(!strcmp(request->method,"GET"))                            
            {
                if( request->host && request->path && (checkHTTPversion(request->version) == 1) )
                {
                    bytes_send_client = handle_request(socket, request, tempReq);
                    if(bytes_send_client == -1)
                    {    
                        sendErrorMessage(socket, 500);
                    }
                }
                else
                    sendErrorMessage(socket, 500);
            }
            else
            {
                printf("This code doesn't support any method other than GET\n");
            }
        }
        ParsedRequest_destroy(request);
    }
    else if( bytes_send_client < 0)
    {
        perror("Error in receiving from client.\n");
    }
    else if(bytes_send_client == 0)
    {
        printf("Client disconnected!\n");
    }

    // Remove thread from scheduler
    remove_thread_from_scheduler(pthread_self());

    shutdown(socket, SHUT_RDWR);
    close(socket);
    custom_free(buffer);
    custom_free(tempReq);
    sem_post(&seamaphore);    
    
    sem_getvalue(&seamaphore,&p);
    printf("Semaphore post value:%d\n",p);
    return NULL;
}

int main(int argc, char * argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int client_socketId, client_len;
    struct sockaddr_in server_addr, client_addr;

    // Initialize mutexes
    sem_init(&seamaphore,0,MAX_CLIENTS);
    pthread_mutex_init(&lock,NULL);
    pthread_mutex_init(&memory_lock,NULL);
    
    // Initialize custom memory allocator
    init_memory_pool();
    printf("Custom memory allocator initialized with %d MB pool\n", MEMORY_POOL_SIZE/(1024*1024));
    
    // Initialize CPU scheduler
    init_scheduler();

    if(argc == 2)
    {
        port_number = atoi(argv[1]);
    }
    else
    {
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Setting Proxy Server Port : %d\n",port_number);

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

    if( proxy_socketId < 0)
    {
        perror("Failed to create socket.\n");
        exit(1);
    }

    int reuse =1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) 
        perror("setsockopt(SO_REUSEADDR) failed\n");

    bzero((char*)&server_addr, sizeof(server_addr));  
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if( bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0 )
    {
        perror("Port is not free\n");
        exit(1);
    }
    printf("Binding on port: %d\n",port_number);

    int listen_status = listen(proxy_socketId, MAX_CLIENTS);

    if(listen_status < 0 )
    {
        perror("Error while Listening !\n");
        exit(1);
    }

    int i = 0;
    int Connected_socketId[MAX_CLIENTS];

    while(1)
    {
        bzero((char*)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr); 

        client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr,(socklen_t*)&client_len);
        if(client_socketId < 0)
        {
            fprintf(stderr, "Error in Accepting connection !\n");
            exit(1);
        }
        else{
            Connected_socketId[i] = client_socketId;
        }

        struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop( AF_INET, &ip_addr, str, INET_ADDRSTRLEN );
        printf("Client is connected with port number: %d and ip address: %s \n",ntohs(client_addr.sin_port), str);
        pthread_create(&tid[i],NULL,thread_fn, (void*)&Connected_socketId[i]);
        i++; 
    }
    
    // Cleanup
    scheduler_running = 0;
    pthread_join(scheduler_tid, NULL);
    close(proxy_socketId);
    return 0;
}

cache_element* find(char* url){
    cache_element* site=NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Remove Cache Lock Acquired %d\n",temp_lock_val); 
    if(head!=NULL){
        site = head;
        while (site!=NULL)
        {
            if(!strcmp(site->url,url)){
                printf("LRU Time Track Before : %ld", site->lru_time_track);
                printf("\nurl found\n");
                site->lru_time_track = time(NULL);
                printf("LRU Time Track After : %ld", site->lru_time_track);
                break;
            }
            site=site->next;
        }       
    }
    else {
        printf("url not found\n");
    }
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Remove Cache Lock Unlocked %d\n",temp_lock_val); 
    return site;
}

void remove_cache_element(){
    cache_element * p ;
    cache_element * q ;
    cache_element * temp;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Remove Cache Lock Acquired %d\n",temp_lock_val); 
    if( head != NULL) {
        for (q = head, p = head, temp =head ; q -> next != NULL; 
            q = q -> next) {
            if(( (q -> next) -> lru_time_track) < (temp -> lru_time_track)) {
                temp = q -> next;
                p = q;
            }
        }
        if(temp == head) {
            head = head -> next;
        } else {
            p->next = temp->next;    
        }
        cache_size = cache_size - (temp -> len) - sizeof(cache_element) - 
        strlen(temp -> url) - 1;
        custom_free(temp->data);
        custom_free(temp->url);
        custom_free(temp);
    } 
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Remove Cache Lock Unlocked %d\n",temp_lock_val); 
}

int add_cache_element(char* data,int size,char* url){
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Add Cache Lock Acquired %d\n", temp_lock_val);
    int element_size=size+1+strlen(url)+sizeof(cache_element);
    if(element_size>MAX_ELEMENT_SIZE){
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        return 0;
    }
    else
    {   
        while(cache_size+element_size>MAX_SIZE){
            remove_cache_element();
        }
        cache_element* element = (cache_element*) custom_malloc(sizeof(cache_element));
        element->data= (char*)custom_malloc(size+1);
        strcpy(element->data,data); 
        element -> url = (char*)custom_malloc(1+( strlen( url )*sizeof(char)  ));
        strcpy( element -> url, url );
        element->lru_time_track=time(NULL);
        element->next=head; 
        element->len=size;
        head=element;
        cache_size+=element_size;
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        return 1;
    }
    return 0;
}
