#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/times.h>

// Define the colors for the output
#define KGRN "\033[0;32;32m"
#define KCYN "\033[0;36m"
#define KRED "\033[0;32;31m"
#define KYEL "\033[1;33m"
#define KBLU "\033[0;32;34m"
#define KCYN_L "\033[1;36m"
#define KBRN "\033[0;33m"
#define RESET "\033[0m"

#define RAM_SIZE 10000
#define SUB_NUM 4

static volatile int activeClient = 1;
static int connection_flag = 0;
static int writeable_flag = 0;

// Threads
pthread_t schedulerThread, calculaterThread;
pthread_t consumer2;
// Timestamp
unsigned long int ideal_minute_timestamp;

// Function to handle the change of the activeClient boolean
static void INT_HANDLER(int signo)
{
    activeClient = 0;
}

// Function-Thread to stop the client
void* timerFunction(void* arg) {
    // 48 hours and 15 minutes
    sleep(48 * 60 * 60 + 15 * 60);
    printf(KRED "Time is up! Terminating the client...\n" RESET);  
    activeClient = 0;
    return NULL;
}

// Time struct
struct timeval t1, t2;


// The JSON paths/labels that we are interested in
static const char *const tok[] = {

    "data[].p", // The price
    "data[].s", // The trade symbol
    "data[].t", // The timestamp
    "data[].v", // The volume

};

//###########DATA STRUCTURES####################
const char *subSymbols[] = {"AAPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"}; 

typedef struct {
    float startingPrice;
    float endingPrice;
    float highestPrice;
    float lowestPrice;
    float totalVolume;
}candlestick;

typedef struct workFunction{
    void (*work)(unsigned long int, unsigned long int, unsigned long int);
    unsigned long int arg1;
    unsigned long int arg2;
    unsigned long int arg3;
} workFunc;

typedef struct {
    float price;
    char symbol[40];
    unsigned long int timestamp;
    float volume;
}Data;

typedef struct {
    float movingAverage;
    float totalVolume;
} AverageVolume;

//###########################QUEUES####################################

// Queue in which are stored the work functions to be executed with their arguments
typedef struct {
    workFunc queueBuffer[RAM_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t *mutex;
    pthread_cond_t *not_empty;
    pthread_cond_t *not_full;
} WorkQueue;

// Queue in which are stored the data from the server -> RAM
typedef struct {
    Data dataBuffer[RAM_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t *mutex;
    pthread_cond_t *not_empty;
    pthread_cond_t *not_full;
} DataQueue;

// Queue in which are stored the moving averages and total volumes for each symbol for the last 15 minutes
typedef struct {
    // Array of moving average and total volume for 15 minutes for each symbol
    AverageVolume mavgBuffer[15][SUB_NUM];
    int capacity; // Max capacity of the queue
    int head; // Head of the queue
    int tail; // Tail of the queue
    int size; // Number of elements in the queue
} movingAverageQueue;

//* Initializers for the queues
WorkQueue *w_queue_init(void) {
    // Allocate memory for the queue
    WorkQueue *q;
    q = (WorkQueue *)malloc(sizeof(WorkQueue));
    if (q == NULL) {
        return NULL;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;

    // Allocate memory for mutex and condition variables
    q->mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    q->not_empty = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    q->not_full = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));

    // Initialize mutex and condition variables
    pthread_mutex_init(q->mutex, NULL);
    pthread_cond_init(q->not_empty, NULL);
    pthread_cond_init(q->not_full, NULL);
    return q;
}

DataQueue *d_queue_init(void) {
    // Allocate memory for the queue
    DataQueue *q;
    q = (DataQueue *)malloc(sizeof(DataQueue));
    if (q == NULL) {
        return NULL;
    }
    q->head = 0;
    q->tail = 0;
    q->count = 0;

    // Allocate memory for mutex and condition variables
    q->mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    q->not_empty = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    q->not_full = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));

    // Initialize mutex and condition variables
    pthread_mutex_init(q->mutex, NULL);
    pthread_cond_init(q->not_empty, NULL);
    pthread_cond_init(q->not_full, NULL);
    return q;
}

movingAverageQueue* m_queue_init(void) {
    // Dynamically allocate memory for the queue
    movingAverageQueue* queue = (movingAverageQueue*)malloc(sizeof(movingAverageQueue));
    if (queue == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }

    // Initialize the queue's members
    queue->capacity = 15; // Max capacity is fixed at 15
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;

    // Initialize the mavgBuffer with zeros (or appropriate initial values)
    for (int i = 0; i < queue->capacity; i++) {
        for (int j = 0; j < SUB_NUM; j++) {
            queue->mavgBuffer[i][j].movingAverage = 0.0;
            queue->mavgBuffer[i][j].totalVolume = 0.0;
        }
    }

    return queue; // Return the pointer to the initialized queue
}
//* Enqueues
void enqueueWork(WorkQueue *q, workFunc value) {
    // Add the element to the current tail position
    q->queueBuffer[q->tail] = value;
    // Move the tail
    q->tail = (q->tail + 1) % RAM_SIZE;
    // If the queue was already full, move the head (head is overwritten)
    if (q->count == RAM_SIZE) {
        q->head = (q->head + 1) % RAM_SIZE;
    } else {
        q->count++;
    }
    pthread_cond_signal(q->not_empty);
}

void enqueueData(DataQueue *q, Data value) {
    // Add the element to the current tail position
    q->dataBuffer[q->tail] = value;
    q->tail = (q->tail + 1) % RAM_SIZE;
    // If the queue was already full, move the head (head is overwritten)
    if (q->count == RAM_SIZE) {
        q->head = (q->head + 1) % RAM_SIZE;
    } else {
        q->count++;
    }
    pthread_cond_signal(q->not_empty);
}

// Enqueue for the moving average queue (when the queue is full, the head is overwritten)
void enqueueMavg(movingAverageQueue *q, AverageVolume values[SUB_NUM]) {
    // Add the SUB_NUM AverageVolume elements to the current tail position
    for (int i = 0; i < SUB_NUM; i++) {
        q->mavgBuffer[q->tail][i] = values[i];
    }
    q->tail = (q->tail + 1) % q->capacity;
    // If the queue was already full, move the head (head is overwritten)
    if (q->size == q->capacity) {
        q->head = (q->head + 1) % q->capacity;
    } else {
        q->size++;
    }
}

// Global variables for the queues
WorkQueue *w_queue;
DataQueue *d_queue;
movingAverageQueue *mavg_queue;

// Check if the Work queue is full
bool queue_is_full(WorkQueue *q) {
    return q->count == RAM_SIZE;
}

// The program does not use other dequeuers since the queue is used as a circular buffer and the head is overwritten
// Dequeue for the working queue
void dequeueWork(WorkQueue *q, workFunc *workOut) {
    *workOut = q->queueBuffer[q->head];
    // Move the head
    q->head = (q->head + 1) % RAM_SIZE;
    q->count--;
    pthread_cond_signal(q->not_full);
}

// Cleanup the queues
void d_queue_destroy(DataQueue *q) {
    // Destroy the mutex and condition variables
    pthread_mutex_destroy(q->mutex);
    pthread_cond_destroy(q->not_empty);
    pthread_cond_destroy(q->not_full);
    // Free the dynamically allocated memory
    free(q->mutex);
    free(q->not_empty);
    free(q->not_full);
    free(q);
}
void w_queue_destroy(WorkQueue *q) {
    // Destroy the mutex and condition variables
    pthread_mutex_destroy(q->mutex);
    pthread_cond_destroy(q->not_empty);
    pthread_cond_destroy(q->not_full);
    // Free the dynamically allocated memory
    free(q->mutex);
    free(q->not_empty);
    free(q->not_full);
    free(q);
}
void m_queue_destroy(movingAverageQueue *q) {
    // Free the dynamically allocated memory
    free(q);
}

//###########################ENDQUEUE#################################

// Definitions of the functions
void *scheduler(void *arg);
void calculations(unsigned long int head, unsigned long int tail, unsigned long int time);
unsigned long int printToFile(Data data);
void *consumer(void *arg);
void tradeDelayCalculation(unsigned long int timestamp, unsigned long int receive_time, unsigned long int finished_writing_time);

// Function to calculate time that the CPU is on idle mode
void get_cpu_idle_time(unsigned long long *idle_time, unsigned long long *total_time) {
    FILE *fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        perror("fopen");
        exit(1);
    }
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    fscanf(fp, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    *idle_time = idle;
    *total_time = user + nice + system + idle + iowait + irq + softirq + steal;
    fclose(fp);
}

// The callback function will parse the JSON data each field at a time and then store it in the buffer
static signed char
cb(struct lejp_ctx *ctx, char reason){
    // Get the current time - store as t1 which represents the time when the message is received
    gettimeofday (&t1, NULL);
    double receive_time = (double)(t1.tv_usec/1.0e6 + t1.tv_sec);
    // Convert to milliseconds
    unsigned long int receive_time_msec = (unsigned long int)(receive_time * 1000);

    static Data data;

    if (reason & LEJP_FLAG_CB_IS_VALUE && (ctx->path_match > 0))
    {
        if ((ctx->path_match) == 1)
        {   
            data.price = atof(ctx->buf);
        }
        else if ((ctx->path_match) == 2)
        {
            strcpy(data.symbol, ctx->buf);
        }
        else if ((ctx->path_match) == 3)
        {  
            data.timestamp = strtoul(ctx->buf, NULL,10);
        }
        else if ((ctx->path_match) == 4)
        {
            data.volume = atof(ctx->buf);
            // Add to the queue
            pthread_mutex_lock(d_queue->mutex);
            //! I wont check if the queue is full because if it is full, the head is beeing overwritten
            unsigned long int finished_writing_time_ms = printToFile(data);
            enqueueData(d_queue, data);
            pthread_cond_signal(d_queue->not_empty);
            pthread_mutex_unlock(d_queue->mutex);

            // Add the trade delay calculation to the work queue
            workFunc delayworkpoint;
            delayworkpoint.work = tradeDelayCalculation;
            delayworkpoint.arg1 = data.timestamp;
            delayworkpoint.arg2 = receive_time_msec;
            delayworkpoint.arg3 = finished_writing_time_ms;

            pthread_mutex_lock(w_queue->mutex);
            while (queue_is_full(w_queue)) {
                pthread_cond_wait(w_queue->not_full, w_queue->mutex);
            }
            enqueueWork(w_queue, delayworkpoint);
            pthread_cond_signal(w_queue->not_empty);
            pthread_mutex_unlock(w_queue->mutex);
        }
    }
    return 0;
}

// Function to calculate the delays at the candlestick and moving average calculations
// And write them to a file
void candleStickDelayCalculation(unsigned long int onMinuteTimestamp, unsigned long int finishedCandlestickTimestamp, unsigned long int finishedAverageTimestamp){
    // Write the timestamps to a file
    char filePath[100];
    snprintf(filePath, sizeof(filePath), "OnMinuteTimestamps.txt");
    FILE *file = fopen(filePath, "a");
    if (file == NULL) {
        perror(KRED "Error opening file" RESET);
        return;
    }
    fprintf(file, "Timestamp when the calculation should have been done: %lu,\t Timestamp when the calculation actually started: %lu,\t Timestamp when the candlestick was written: %lu,\t Timestamp when the moving average was written: %lu\n", ideal_minute_timestamp, onMinuteTimestamp, finishedCandlestickTimestamp, finishedAverageTimestamp); 
    fclose(file);
    // Write the delays to another file
    unsigned long int candlestick_delay = finishedCandlestickTimestamp - onMinuteTimestamp;
    unsigned long int average_delay = finishedAverageTimestamp - onMinuteTimestamp;
    unsigned long int drift = onMinuteTimestamp - ideal_minute_timestamp;
    char filePath2[100];
    snprintf(filePath2, sizeof(filePath2), "OnMinuteDelays.txt");
    FILE *file2 = fopen(filePath2, "a");
    if (file2 == NULL) {
        perror(KRED "Error opening file" RESET);
        return;
    }
    fprintf(file2, "Candlestick delay: %lums,\t Moving average delay: %lums\t, Drift: %lums\n", candlestick_delay, average_delay, drift);
    fclose(file2);
    // Update the ideal_minute_timestamp for the next minute
    ideal_minute_timestamp += 60000; // Add 60 seconds for the next minute ideal timestamp without drift
}

// Function to calculate the delays at the trade calculations
void tradeDelayCalculation(unsigned long int timestamp, unsigned long int receive_time, unsigned long int finished_writing_time){
    unsigned long int finnhub_delay = receive_time - timestamp; // The delay between the timestamp and the time the message is received+parsed
    unsigned long int writing_delay = finished_writing_time - receive_time; // The delay between the time the message is received+parsed and the time the message is written to the file
    // Write the delays to the file
    char filePath[100];
    snprintf(filePath, sizeof(filePath), "TradeDelays.txt");
    FILE *file = fopen(filePath, "a");
    if (file == NULL) {
        perror(KRED "Error opening file" RESET);
        return;
    }
    fprintf(file, "Server delay until receiving: %lums,\t Writing delay: %lums\n", finnhub_delay, writing_delay);
    //fprintf(file, "Server delay until receiving: %lums\t Writing delay: %lums\t, timestamp: %lu\t, receive_time: %lu\t, finished_writing_time: %lu\n", finnhub_delay, writing_delay, timestamp, receive_time, finished_writing_time);
    fclose(file);
}

// Consumer function
// Purpose: at the end of each minute find the current head and tail of the queue
// and assign add this to the work queue
void *scheduler(void *arg) {
    // Initialize the front index and tail index (for the first iteration)
    int front_index = d_queue->head;
    int tail_index = d_queue->tail;
    workFunc workpoint;
    struct timeval calcTime;
    unsigned long int calc_time_ms;
    bool firstItteration = true;
    // This function will run indefinitely
    while (activeClient) {
        sleep(60);
        // Get the current time
        gettimeofday(&calcTime, NULL);
        // Only for the first iteration, store the time of the first calculation to know the time that the calculations
        // should be called without a drift
        if (firstItteration) {
            ideal_minute_timestamp = (unsigned long int)((calcTime.tv_usec/1.0e6 + calcTime.tv_sec)*1000);
            firstItteration = false;
        }
        calc_time_ms = (unsigned long int)((calcTime.tv_usec/1.0e6 + calcTime.tv_sec)*1000);
        // After one minute has passed, find the tail of the queue
        tail_index = d_queue->tail;
        // Assign to the work point the function to be executed and the head and tail of the queue
        workpoint.work = calculations;
        workpoint.arg1 = front_index;
        workpoint.arg2 = tail_index;
        workpoint.arg3 = calc_time_ms;

        pthread_mutex_lock(w_queue->mutex);
        while (queue_is_full(w_queue)) {
            pthread_cond_wait(w_queue->not_full, w_queue->mutex);
        }

        enqueueWork(w_queue, workpoint);
        pthread_cond_signal(w_queue->not_empty);
        pthread_mutex_unlock(w_queue->mutex);
        
        // Initialization for the next minute: the front index will be the tail index of 
        // the previous minute, in order to prevent data losses
        front_index = tail_index + 1;
    }
}

void *consumer(void *arg){
    workFunc workpoint;
    while(activeClient){
        pthread_mutex_lock(w_queue->mutex);
        while (w_queue->count == 0) {
            pthread_cond_wait(w_queue->not_empty, w_queue->mutex);
        }
        // Dequeue the first task from the working queue
        dequeueWork(w_queue, &workpoint);
        workpoint.work(workpoint.arg1, workpoint.arg2, workpoint.arg3);
        pthread_cond_signal(w_queue->not_full);
        pthread_mutex_unlock(w_queue->mutex);
    }
    return (NULL);
}

// Initialize the candlestick
void candlestick_init(candlestick *candlestick){
    candlestick->startingPrice = 0;
    candlestick->endingPrice = 0;
    candlestick->highestPrice = __FLT_MIN__;
    candlestick->lowestPrice = __FLT_MAX__;
    candlestick->totalVolume = 0;
}

// This function *will be called every minute* to calculate the candlesticks
// The head and the tail of the part of the queue that we are interested
void calculations(unsigned long int head, unsigned long int tail, unsigned long int time){
    // printf(KGRN "Calculating candlesticks...\n" RESET);
    Data data; // Variable to store the data from the d_queue
    candlestick candlesticks[SUB_NUM]; // Candlesticks for each symbol for 1 minute
    float movingAverageFifteen[SUB_NUM] = {0}; // Moving average for the last 15 minutes for each symbol
    float totalVolumeFifteen[SUB_NUM] = {0}; // Total volume for the last 15 minutes for each symbol
    int minutesInQueue[SUB_NUM] = {0}; // Size of the mavg_queue (if there aren't moving averages for 15 minutes yet)
    float sum[SUB_NUM] = {0}; // Sum of the prices for the moving mean
    unsigned long int totalTransactions[SUB_NUM] = {0}; // Total number of transactions for each symbol for the calculation of the moving mean
    
    // Time variables for the delay calculations
    struct timeval candlTime, mavgTime;
    // Initialize the candlesticks
    for (int i = 0; i < SUB_NUM; i++) {
        candlestick_init(&candlesticks[i]);
    }
    AverageVolume average[SUB_NUM]; // Variable to store the moving average and total volume for each symbol for the current minute

    // Loop through the buffer until the last index
    for(; head != tail; head = (head + 1) % RAM_SIZE){
        //* Without dequeuing take the wanted element from the queue
        data = d_queue->dataBuffer[head];
        // For each symbol, first check if the symbol is the same as the current-one (front+shift) in the buffer
        for(int i = 0; i < SUB_NUM; i++){
            if(strcmp(data.symbol, subSymbols[i]) == 0)
            {
                // If the starting price is 0, set it to the current price
                if(candlesticks[i].startingPrice == 0)
                {
                    candlesticks[i].startingPrice = data.price;
                }
                // If the current price is higher than the highest price, set it as the highest price
                if(data.price > candlesticks[i].highestPrice)
                {
                    candlesticks[i].highestPrice = data.price;
                }
                // If the current price is lower than the lowest price, set it as the lowest price
                if(data.price < candlesticks[i].lowestPrice)
                {
                    candlesticks[i].lowestPrice = data.price;
                }
                // Add the volume to the total volume
                candlesticks[i].totalVolume += data.volume;
                // Increment the number of transactions
                totalTransactions[i]++;
                // At the end of the minute, at this position we will have the final price
                candlesticks[i].endingPrice = data.price;
                // Calculate the sum of the prices for the moving mean
                sum[i] += data.price;
            }
        }
    }
    // Save the candlestick calculations to the file
    for (int i = 0; i < SUB_NUM; i++)
    {
        // Open the file
        char fileName[100];
        snprintf(fileName, sizeof(fileName), "%s/%s_candlesticks_minute.txt", subSymbols[i], subSymbols[i]);
        FILE *file = fopen(fileName, "a");
        // Write the data to the file
        fprintf(file, "Starting Price: %f, Ending Price: %f, Highest Price: %f, Lowest Price: %f, Total Volume: %f\n", candlesticks[i].startingPrice, candlesticks[i].endingPrice, 
                        candlesticks[i].highestPrice, candlesticks[i].lowestPrice, 
                        candlesticks[i].totalVolume);
        // Close the file
        fclose(file);
    }
    // Timestamp for the end of the candlestick calculations
    gettimeofday(&candlTime, NULL);
    unsigned long int finishedCandlestickTimestamp = (unsigned long int)((candlTime.tv_usec/1.0e6 + candlTime.tv_sec)*1000);

    // Calculate the moving mean price for each symbol for this minute
    for (int i = 0; i < SUB_NUM; i++)
    {
        average[i].totalVolume = candlesticks[i].totalVolume;
        average[i].movingAverage = sum[i] / totalTransactions[i];
        // If the highest price is the minimum float value or the lowest price is the maximum float value, we dont have any data
        if (candlesticks[i].highestPrice == __FLT_MIN__ || candlesticks[i].lowestPrice == __FLT_MAX__)
        {
            average[i].totalVolume = 0.0;
            average[i].movingAverage = 0.0;
        }
    }

    // Add the moving average and total volume to the queue
    enqueueMavg(mavg_queue, average);

    // Calculate the moving average and total volume based on the data of the last 15 minutes
    for(int i = 0; i < SUB_NUM; i++){
        // Initialize
        minutesInQueue[i] = 0;
        // Sum the total volume and the moving average of the last 15 minutes
        for(int j = 0; j < 15; j++){
            totalVolumeFifteen[i] += mavg_queue->mavgBuffer[j][i].totalVolume;
            movingAverageFifteen[i] += mavg_queue->mavgBuffer[j][i].movingAverage;
            if (mavg_queue->mavgBuffer[j][i].movingAverage != 0.0) {
                minutesInQueue[i]++;
            }
        }
        // In order to calculate the average, we divide by the number of minutes in the queue (because for the starting 15 minutes we dont have 15 data points)
        movingAverageFifteen[i] = movingAverageFifteen[i] / minutesInQueue[i];
        if (minutesInQueue[i] == 0) {
            movingAverageFifteen[i] = 0.0;
        }
        // Open the file and write the moving average and the total volume
        char fileName[100];
        snprintf(fileName, sizeof(fileName), "%s/%s_15minutes_average.txt", subSymbols[i], subSymbols[i]);
        FILE *file = fopen(fileName, "a");
        fprintf(file, "Average trade price: %f, Total volume: %f\n", movingAverageFifteen[i], totalVolumeFifteen[i]);
        fclose(file);
    }
    // Timestamp for the end of the moving average calculations
    gettimeofday(&mavgTime, NULL);
    unsigned long int finishedAverageTimestamp = (unsigned long int)((mavgTime.tv_usec/1.0e6 + mavgTime.tv_sec)*1000);
    // Call the function to calculate the delays
    candleStickDelayCalculation(time, finishedCandlestickTimestamp, finishedAverageTimestamp);
}


// This is happening the moment the message is received
// Covers the 1st request
unsigned long int printToFile(Data data){
    // Construct the file path based on the symbol
    char filePath[100];
    snprintf(filePath, sizeof(filePath), "%s/%s_trading_infos.txt", data.symbol, data.symbol);

    // Open the file in append mode
    FILE *file = fopen(filePath, "a");
    if (file == NULL) {
        perror("Error opening file");
        return -1;
    }
    // Write the data to the file
    fprintf(file, "Price: %f, Symbol: %s, Timestamp: %lu, Volume: %f\n", data.price, data.symbol, data.timestamp, data.volume);
    // Close the file
    fclose(file);

    // Get the current time - store as t2 which represents the time when the message is done with the writing to the file
    gettimeofday (&t2, NULL);
    double finished_writing_time = (double)(t2.tv_usec/1.0e6 + t2.tv_sec);
    // Convert to milliseconds
    unsigned long int finished_writing_time_msec = (unsigned long int)(finished_writing_time * 1000);
    return finished_writing_time_msec;
}


// Function to "write" to the socket in order to send messages to the server
static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in)
{
    if (str == NULL || wsi_in == NULL)
        return -1;
    int m;
    int n;
    int len;
    char *out = NULL;

    if (str_size_in < 1)
        len = strlen(str);
    else
        len = str_size_in;

    out = (char *)malloc(sizeof(char) * (LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
    //* setup the buffer*/
    memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len);
    //* write out*/
    n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

    printf(KBLU "[websocket_write_back] %s\n" RESET, str);
    //* free the buffer*/
    free(out);
    return n;
}

void unsubscribe_from_all(struct lws *wsi) {
    char symbols[4][50] = {"AAPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"};
    for (int i = 0; i < 4; i++) {
            char buffer[128];
            sprintf(buffer, "{\"type\":\"unsubscribe\",\"symbol\":\"%s\"}", symbols[i]);
            websocket_write_back(wsi, buffer, strlen(buffer));
    }
}
struct lejp_ctx ctx;

// The websocket callback function
static int ws_service_callback(
    struct lws *wsi,
    enum lws_callback_reasons reason, void *user,
    void *in, size_t len)
{
    // Switch-Case structure to check the reason for the callback
    switch (reason)
    {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        // 2) When the connection is established, print
        printf(KYEL "[Main Service] Connect with server success.\n" RESET);
        sleep(5); // Delay before sending the subscribe messages
        // Call the on writable callback, to send the subscribe messages to the server
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf(KRED "[Main Service] Connect with server error: %s.\n" RESET, in);
        // Set the flag to 0, to show that the connection was lost
        connection_flag = 0;
        lejp_destruct(&ctx);
        unsubscribe_from_all(wsi);
        if (activeClient)
        {
            sleep(25);  // Delay before reconnecting (5 seconds)
            lws_cancel_service(lws_get_context(wsi));
        }
        break;

    case LWS_CALLBACK_CLOSED:
        printf(KYEL "[Main Service] LWS_CALLBACK_CLOSED\n" RESET);
        // Set the flag to 0, to show that the connection was lost
        connection_flag = 0;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:;
        // Incoming messages are handled here
        fflush(stdout);
        // Initialize a LEJP JSON parser, and pass it the incoming message
        char *msg = (char *)in;
        char print_msg[70];
        strncpy(print_msg, msg, 70);
        print_msg[69] = '\0';
        printf(KCYN "[Main Service] Received data: %s\n" RESET, print_msg);
        //printf(KCYN "[Main Service] Received data: %s\n" RESET, msg);
        // Enqueue the incoming message to the queue - the consumer thread will process it
        // struct lejp_ctx ctx; // the context of the parser
        lejp_construct(&ctx, cb, NULL, tok, LWS_ARRAY_SIZE(tok)); // construct the parser
        int m = lejp_parse(&ctx, (uint8_t *)msg, strlen(msg)); // parse the message
        if (m < 0 && m != LEJP_CONTINUE)
        {
            lwsl_err("parse failed %d\n", m);
        }
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        // 3) The first step is to send the subscribe messages to the server
        // When writeable, send the server the desired trade symbols to subscribe to, if not already subscribed
        if (!writeable_flag)
        {
            printf(KYEL "\n[Main Service] On writeable is called.\n" RESET);
            char symb_arr[4][50] = {"AAPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"};
            char str[100];
            for (int i = 0; i < 4; i++)
            {
                sprintf(str, "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symb_arr[i]);
                int len = strlen(str);
                websocket_write_back(wsi, str, len);
            }
            // Set the flag to 1, to show that the subscribe request have been sent
            writeable_flag = 1;
        }
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        // If the client is closed for some reason, set the connection and writeable flags to 0,
        // so a connection can be re-established
        printf(KYEL "\n[Main Service] Client closed %s.\n" RESET, in);
        // printf("\a");  // ASCII Bell character
        // fflush(stdout);
        connection_flag = 0;
        writeable_flag = 0;
        lejp_destruct(&ctx);
        unsubscribe_from_all(wsi);
        if (activeClient)
        {
            sleep(25);  // Delay before reconnecting (5 seconds)
            lws_cancel_service(lws_get_context(wsi));
        }
        break;
    default:
        break;
    }
    return 0;
}

// Protocol to be used with the websocket callback
static struct lws_protocols protocols[] =
    {
        {
            "trade_protocol",
            ws_service_callback,
        },
        {NULL, NULL, 0, 0} /* terminator */
};

// Main function
int main(void)
{
    struct tms time_sample;
    clock_t clock_time = times(&time_sample);

    pthread_t timerThread;
    // Create a threads
    pthread_create(&timerThread, NULL, timerFunction, NULL);

    printf(KGRN "[Main] Threads created.\n" RESET);
    unsigned long long idle_time1, total_time1;
    get_cpu_idle_time(&idle_time1, &total_time1);
    // Set intHandle to handle the SIGINT signal
    // (Used for terminating the client)
    // Also write the idle time percentage to a file
    char filePath[100];
    snprintf(filePath, sizeof(filePath), "idle_time_percentage.txt");
    FILE *file3 = fopen(filePath, "a");
    if (file3 == NULL) {
        perror("Error opening file");
        return -1;
    }
    fprintf(file3, "Idle time1: %llu, Total time1: %llu\n", idle_time1, total_time1);
    fclose(file3);
    signal(SIGINT, INT_HANDLER);

    // Create a folder for each symbol subscription
    for (int i = 0; i < 4; i++) {
        // Create the directory for each symbol
        if (mkdir(subSymbols[i], 0700) == 0) {
            printf("Directory created: %s\n", subSymbols[i]);
        } else {
            perror("mkdir failed");
        }
    }

    // Initialize the queues
    w_queue = w_queue_init();
    d_queue = d_queue_init();
    mavg_queue = m_queue_init();

    // Set the LWS and its context
    struct lws_context *context = NULL;
    struct lws_context_creation_info info;
    struct lws *wsi = NULL;
    struct lws_protocols protocol;
    
    memset(&info, 0, sizeof info);

    // Set the context of the websocket
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    //! Set the path to the certificate file because on my machine i got a cert error
    info.client_ssl_ca_filepath = "/etc/ssl/certs/ca-certificates.crt";

    // Set the Finnhub url
    char *api_key = "crbn15hr01qtpc70tqagcrbn15hr01qtpc70tqb0";
    if (strlen(api_key) == 0)
    {
        printf(" API KEY NOT PROVIDED!\n");
        return -1;
    }

    // Create the websocket context
    context = lws_create_context(&info);
    printf(KGRN "[Main] context created.\n" RESET);
    if (context == NULL)
    {
        printf(KRED "[Main] context is NULL.\n" RESET);
        return -1;
    }
    // Set up variables for the url
    char inputURL[300];
    sprintf(inputURL, "wss://ws.finnhub.io/?token=%s", api_key);
    const char *urlProtocol, *urlTempPath;
    char urlPath[300];
    struct lws_client_connect_info clientConnectionInfo;
    memset(&clientConnectionInfo, 0, sizeof(clientConnectionInfo));

    // Set the context for the client connection
    clientConnectionInfo.context = context;
    // Parse the url
    if (lws_parse_uri(inputURL, &urlProtocol, &clientConnectionInfo.address,
                      &clientConnectionInfo.port, &urlTempPath))
    {
        printf("Couldn't parse URL\n");
    }
    urlPath[0] = '/';
    strncpy(urlPath + 1, urlTempPath, sizeof(urlPath) - 2);
    urlPath[sizeof(urlPath) - 1] = '\0';

    // While a kill signal is not sent (ctrl+c), the client is active
    pthread_create(&schedulerThread, NULL, scheduler, NULL);
    pthread_create(&calculaterThread, NULL, consumer, NULL);
    pthread_create(&consumer2, NULL, consumer, NULL);
    while (activeClient)
    {   
        // 1) Connect to the server
        // If the websocket is not connected, connect
        if (!connection_flag || !wsi)
        {
            // Set the client information
            connection_flag = 1;
            clientConnectionInfo.port = 443;
            clientConnectionInfo.path = urlPath;
            clientConnectionInfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED;
            //! Altreatively (from the cert), you can disable the SSL verification - But got an error with this way
            //clientConnectionInfo.ssl_connection =  | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

            clientConnectionInfo.host = clientConnectionInfo.address;
            clientConnectionInfo.origin = clientConnectionInfo.address;
            clientConnectionInfo.ietf_version_or_minus_one = -1;
            clientConnectionInfo.protocol = protocols[0].name;

            printf(KGRN "Connecting to %s://%s:%d%s \n\n" RESET, urlProtocol,
                   clientConnectionInfo.address, clientConnectionInfo.port, urlPath);

            wsi = lws_client_connect_via_info(&clientConnectionInfo);
            if (wsi == NULL)
            {
                printf(KRED "[Main] wsi create error.\n" RESET);
                return -1;
            }

            printf(KGRN "[Main] wsi creation success.\n" RESET);
        }
        // 3) Service any pending websocket activity immediately
        lws_service(context, 0);
    }
    printf(KRED "\n[Main] Closing client\n" RESET);
    lws_context_destroy(context);
    sleep(2);
        // Calculate the CPU idle time percentage
    unsigned long long idle_time2, total_time2;
    get_cpu_idle_time(&idle_time2, &total_time2);

    double idle_time_diff;
    idle_time_diff = (double)(idle_time2 - idle_time1);
    double total_time_diff;
    total_time_diff = (double)(total_time2 - total_time1);
    double idle_percentage;
    idle_percentage = (idle_time_diff / total_time_diff) * 100.0;
    printf("Idle time percentage: %.2f%%\n", idle_percentage);

    // Join the threads
    pthread_join(timerThread, NULL);
    pthread_join(calculaterThread, NULL);
    pthread_join(schedulerThread, NULL);
    pthread_join(consumer2, NULL);
    
    w_queue_destroy(w_queue);
    d_queue_destroy(d_queue);
    m_queue_destroy(mavg_queue);
    printf(KRED "\n[Main] Queues destroyed\n" RESET);

    sleep(2);

    // Exit, gracefully
    return 0;
}