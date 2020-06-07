// import necessary libraries
#include <sys/types.h>      // socket
#include <sys/socket.h>     // socket
#include <string.h>         // memset
#include <stdlib.h>         // sizeof
#include <netinet/in.h>     // INADDR_ANY
#include <stdio.h>          
#include <arpa/inet.h>     
#include <netdb.h>         
#include <errno.h>          // interpreting errors
#include <time.h>           // time functions
#include <math.h>           // math functions
#include <pthread.h>        // client handler thread
#include <signal.h>         // signal handling (ctrl+c exception)
#include <ueye.h>           // uEye camera operation

// include our header files
#include "camera.h"
#include "astrometry.h"
#include "lens_adapter.h"
#include "commands.h"

#pragma pack(push, 1)
/* Telemetry and camera settings structure */
struct telemetry {
    struct astrometry astrom;
    struct camera_params cam_settings; 
    struct blob_params current_blob_params;
};
/* User commands structure */
struct commands {
    double logodds;         // parameter for amount of false positives that make it through astrometry
    double latitude;        // user's latitude (radians)
    double longitude;       // user's longitude (degrees)
    double height;          // user's height above ellipsoid from GPS (WGS84)
    double exposure;        // user's desired camera exposure (msec)
    float focus_pos;        // user's desired focus position (counts)
    int focus_mode;         // flag to enter auto-focusing mode (1 = enter focus mode, 0 = leave focus mode)
    int start_focus_pos;    // where to start the auto-focusing process
    int end_focus_pos;      // where to end the auto-focusing process
    int focus_step;         // granularity of auto-focusing checker
    int set_focus_inf;      // 0 = false, 1 = true (whether or not to set the focus to infinity)
    int aperture_steps;     // number of shifts (+/-) needed to reach desired aperture
    int set_max_aperture;   // 0 = false, 1 = true (whether or not to maximize aperture)
    int make_hp;            // flag to make a new static hp mask (0 = don't make, 20 = re-make)
    int use_hp;             // flag to use current static hp mask (0 = don't use, 1 = use)
    float blob_params[9];   // rest of blob-finding parameters (see definition of blob_params struct for explanations)
};
/* Struct for passing arguments to client thread(s) from main thread */
struct args {
    uintptr_t user_socket;
    socklen_t user_length;
    struct sockaddr_in user_address;
    char * user_IP;
};
#pragma pack(pop)

/* Constants */
#define PORT  8000

// initialize instances of these structures (0 as beginning dummy value)
struct commands all_cmds = {0};
struct telemetry all_data = {0};
// initialize pointer for storing address to current camera image bytes (accessible by camera.c)
void * camera_raw;
int command_lock = 0;       // if 1, then commanding is in use; if 0, then not
int shutting_down = 0;      // if 0, then camera is not closing, so keep solving astrometry

/* Helper function for testing reception of user commands.
** Input: Nones.
** Output: None (void). Prints the most recent user commands to the terminal.
*/
void verifyUserCommands() {
    printf("\n**** USER COMMANDS ****\n");
    printf("Logodds command: %f\n", all_cmds.logodds);
    printf("Latitude and longitude commands: %f and %f\n", all_cmds.latitude, all_cmds.longitude);
    printf("Exposure command in commands.c: %f\n", all_cmds.exposure);
    printf("Focusing mode: %s\n", (all_cmds.focus_mode) ? "Auto-focusing" : "Normal focusing");
    printf("Start focus position: %i, end focus position %i, focus step %i\n", all_cmds.start_focus_pos, all_cmds.end_focus_pos, 
                                                                               all_cmds.focus_step);
    printf("Focus position command: %f\n", all_cmds.focus_pos);
    printf("Set focus to infinity bool command: %i\n", all_cmds.set_focus_inf);
    printf("Aperture steps command: %i\n", all_cmds.aperture_steps);
    printf("Set aperture max bool: %i\n", all_cmds.set_max_aperture);
    printf("Make static hp mask: %i and use static hp mask: %i\n", all_cmds.make_hp, all_cmds.use_hp);
    printf("Blob parameters: %f, %f, %f, %f, %f, %f, %f, %f, %f\n", all_cmds.blob_params[0], all_cmds.blob_params[1],
            all_cmds.blob_params[2], all_cmds.blob_params[3], all_cmds.blob_params[4], all_cmds.blob_params[5],
            all_cmds.blob_params[6], all_cmds.blob_params[7], all_cmds.blob_params[8]);
    printf("***********************\n");
}

/* Helper function for testing transmission of telemetry.
** Input: None.
** Output: None (void). Prints the telemetry sent to the user to the terminal.
*/
void verifyTelemetryData() {
    printf("\n**** TELEMETRY ****\n");
    printf("Current rawtime: %f\n", all_data.astrom.rawtime);
    printf("RA: %.15f\n", all_data.astrom.ra);
    printf("DEC: %.15f\n", all_data.astrom.dec);
    printf("FR: %.15f\n", all_data.astrom.fr);
    printf("AZ: %.15f\n", all_data.astrom.az);
    printf("ALT: %.15f\n", all_data.astrom.alt);
    printf("IR: %.15f\n", all_data.astrom.ir);
    printf("PS: %f\n", all_data.astrom.ps);
    printf("Logodds: %f\n", all_data.astrom.logodds);
    printf("Latitude: %.15f\n", all_data.astrom.latitude);
    printf("Longitude: %.15f\n", all_data.astrom.longitude);
    printf("Height: %f\n", all_data.astrom.hm);
    printf("***********************\n");
}

/* Function devoted to taking pictures and solving astrometry while camera is not in a state of shutting down.
** Input: None.
** Output: None (void). 
*/
void * updateAstrometry() {
    // solve astrometry perpetually when the camera is not shutting down
    while (!shutting_down) {
        doCameraAndAstrometry();
    }
}

/* Function for accepting newly connected clients and sending telemetry/receiving their commands.
** Input: An args struct containing the client information.
** Output: None (void).
*/
void * processClient(void * for_client_thread) {
    // get the socket information for this user
    int socket = ((struct args *) for_client_thread)->user_socket;
    int length = ((struct args *) for_client_thread)->user_length;
    struct sockaddr_in address = ((struct args *) for_client_thread)->user_address;
    char * ip_addr = ((struct args *) for_client_thread)->user_IP;
    int recv_status;

    // send data to this client perpetually and receive potential commands if user sends them
    while (1) {
        recv_status = recvfrom(socket, &all_cmds, sizeof(struct commands), 0, (struct sockaddr *) &address, &length); 
        if (recv_status == -1) {
            printf("User %s did not send any commands. Send telemetry and camera settings back anyway.\n", ip_addr);
        } else {
            // if another client is sending commands at the same time, wait until they are done
            while (command_lock) {
                usleep(100000);
            }
            // now it's this client's turn to execute commands (lock)
            command_lock = 1;
            printf("User %s sent commands. Executing...\n", ip_addr);
            verifyUserCommands();

            // update astro params struct with user commands (cmd struct values)
            all_astro_params.logodds = all_cmds.logodds;
            all_astro_params.latitude = all_cmds.latitude;
            all_astro_params.longitude = all_cmds.longitude;
            all_astro_params.hm = all_cmds.height;
            // if user has adjusted the exposure time, set camera exposure to their desired value
            if (ceil(all_cmds.exposure) != ceil(all_camera_params.exposure_time)) {
                // update value in camera params struct as well
                all_camera_params.exposure_time = all_cmds.exposure;
                all_camera_params.change_exposure_bool = 1;
            }

            // pass auto-focusing commands to camera settings struct from commands struct
            all_camera_params.focus_mode = all_cmds.focus_mode;
            all_camera_params.start_focus_pos = all_cmds.start_focus_pos;
            all_camera_params.end_focus_pos = all_cmds.end_focus_pos;
            all_camera_params.focus_step = all_cmds.focus_step;

            // if the command to set the focus to infinity is true (1), ignore any other commands the user might 
            // have put in for focus accidentally
            all_camera_params.focus_inf = all_cmds.set_focus_inf;
            // if user wants to change the focus, change focus position value in camera params struct
            if (all_cmds.focus_pos != -1) {
                all_camera_params.focus_position = all_cmds.focus_pos; 
            } 
                
            // update camera params struct with user commands
            all_camera_params.max_aperture = all_cmds.set_max_aperture;
            all_camera_params.aperture_steps = all_cmds.aperture_steps;

            // perform changes to camera settings in lens_adapter.c (the focus, aperture, and exposure 
            // deal with camera hardware)
            adjustCameraHardware();

            // process the blob parameters
            all_blob_params.make_static_hp_mask = all_cmds.make_hp;               // re-make static hot pixel map with new image (0 = off, 20 = re-make)
            all_blob_params.use_static_hp_mask = all_cmds.use_hp;                 // use the static hot pixel map (0 = off, 1 = on)

            if (all_cmds.blob_params[0] >= 0) {
                all_blob_params.spike_limit = all_cmds.blob_params[0];            // how agressive is the dynamic hot pixel finder.  Smaller is more agressive
            } 
            
            all_blob_params.dynamic_hot_pixels = all_cmds.blob_params[1];         // turn dynamic hot pixel finder 0 = off, 1 = on
        
            if (all_cmds.blob_params[2] >= 0) {
                all_blob_params.r_smooth = all_cmds.blob_params[2];               // image smooth filter radius [px]
            }

            all_blob_params.high_pass_filter = all_cmds.blob_params[3];           // turn high pass filter 0 = off, 1 = on
            
            if (all_cmds.blob_params[4] >= 0) {
                all_blob_params.r_high_pass_filter = all_cmds.blob_params[4];     // image high pass filter radius [px]
            } 
            
            if (all_cmds.blob_params[5] >= 0) {
                all_blob_params.centroid_search_border = all_cmds.blob_params[5]; // distance from image edge from which to start looking for stars [px]
            } 

            all_blob_params.filter_return_image = all_cmds.blob_params[6];        // filter return image 1 = true; 0 = false

            if (all_cmds.blob_params[7] >= 0) {
                all_blob_params.n_sigma = all_cmds.blob_params[7];                // pixels brighter than this time the noise in the filtered map are blobs (this number * sigma + mean)
            } 
            
            if (all_cmds.blob_params[8] >= 0) {
                all_blob_params.unique_star_spacing = all_cmds.blob_params[8];    // minimum pixel spacing between unique stars
            } 

            // allow other clients to execute commands (unlock)
            command_lock = 0; 
        }
        // compile telemetry and transmit back to use
        memcpy(&all_data.astrom, &all_astro_params, sizeof(all_astro_params));
        memcpy(&all_data.cam_settings, &all_camera_params, sizeof(all_camera_params));
        memcpy(&all_data.current_blob_params, &all_blob_params, sizeof(all_blob_params));

        printf("Size of all_data: %lu bytes\n", sizeof(all_data));
        verifyTelemetryData();

        // printf("size of data %lu\n", sizeof(all_data));
        if (send(socket, &all_data, sizeof(struct telemetry), MSG_NOSIGNAL) <= 0) {
            printf("Client dropped the connection.\n");
            break;
        } 

        // send image bytes back to user for image display
        if (send(socket, camera_raw, CAMERA_WIDTH*CAMERA_HEIGHT, MSG_NOSIGNAL) <= 0) {
            printf("Client dropped the connection.\n");
            break;
        }

        printf("Telemetry and image bytes sent back to user.\n");      
    }
    // clean up socket when the connection is done
    close(socket);
}

/* Driver function for Star Camera operation.
** Input: None.
** Output: Flag indicating successful Star Camera operation or an error -> exit.
*/
int main() {
    // signal handling (e.g. ctrl+c exception)
    signal(SIGHUP, clean);
    signal(SIGINT, clean);
    signal(SIGTERM, clean);
    signal(SIGPIPE, SIG_IGN);

    int sockfd;                       // to create socket
    int newsockfd;                    // to accept new connection
    struct sockaddr_in serv_addr;     // server receives on this address
    struct sockaddr_in client_addr;   // server sends to client on this address
    struct timeval read_timeout;      // timeout options for server socket 
    int client_addr_len;              // length of client addresses
    pthread_t client_thread_id;       // thread ID for new client       
    pthread_t astro_thread_id;        // thread ID for Astrometry thread

    printf("Size of all_data: %lu bytes\n", sizeof(all_data));
    printf("--------------------------------\n");

    // create server socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Error creating Star Camera server socket: %s.\n", strerror(errno));
        // the program did not successfully run
        exit(EXIT_FAILURE);
    }

    // initialize the server socket addresses
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT); 

    // bind the server socket with the server address and port
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "Error binding Star Camera server socket to the Star Camera address and port: %s.\n", strerror(errno));
        // the program did not successfully run
        exit(EXIT_FAILURE);
    }

    // listen for connections from clients (maximum is 5 currently)
    if (listen(sockfd, 5) < 0) {
        fprintf(stderr, "Star Camera server unable to listen for clients: %s.\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // make server socket non-blocking (times out after a certain time)
    read_timeout.tv_sec = 2.5;
    read_timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout)) < 0) {
        fprintf(stderr, "Error setting Star Camera server socket timeout: %s.\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // initialize the camera (inputs: 1 = load previous observing data for testing, 0 = take new data)
    if (initCamera(0) < 0) {
        return -1;             // ASK ABOUT THIS (versus exit(EXIT_FAILURE) - always hangs)
    }

    // initialize the lens adapter
    if (initLensAdapter("/dev/ttyLens") < 0) {
        fprintf(stderr, "Error opening file descriptor for lens /dev/ttyLens: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // create a thread separate from all the client thread(s) to just solve astrometry 
    if (pthread_create(&astro_thread_id, NULL, updateAstrometry, NULL) != 0) {
        fprintf(stderr, "Error creating Astrometry thread: %s.\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // loop forever, accepting new clients
    while (newsockfd = accept(sockfd, (struct sockaddr *) &client_addr, &client_addr_len)) {
        // parent process waiting to accept a new connection
        printf("\n******************************* Server waiting for new client connection: *******************************\n");
        // store length of client socket that has connected (if any)
        client_addr_len = sizeof(client_addr);
        if (newsockfd == -1) {
            printf("New client did not connect.\n");
        } else {
            // user did connect, so process their information for their respective client thread
            printf("Connected to client: %s\n", inet_ntoa(client_addr.sin_addr)); 
            struct args * client_args = (struct args *) malloc(sizeof(struct args));
            client_args->user_socket = newsockfd;
            client_args->user_length = client_addr_len;
            client_args->user_address = client_addr;
            client_args->user_IP = inet_ntoa(client_addr.sin_addr);
            // create new thread for this new client
            if (pthread_create(&client_thread_id, NULL, processClient, (void *) client_args) < 0) {
                perror("Could not create thread for new client.\n");
            }
        }
    }

    return 1;
}