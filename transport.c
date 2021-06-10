/*
 * transport.c 
 *
 * COS461: HW#3 (STCP)
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file. 
 *
 */


#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdarg.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"


#define MSS 536
#define WND_LEN 3072
#define INC_SEQ(ctx) ++(ctx->next_seq)

enum { 
    CSTATE_CLOSED,
    CSTATE_LISTEN,
    CSTATE_SYN_RCVD,
    CSTATE_SYN_SENT,
    CSTATE_ESTABLISHED,
    CSTATE_FIN_WAIT_1,
    CSTATE_FIN_WAIT_2,
    CSTATE_TIME_WAIT,
    CSTATE_CLOSING,
    CSTATE_CLOSE_WAIT,
    CSTATE_LAST_ACK 
};    /* obviously you should have more states */


/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done;    /* TRUE once connection is closed */

    int connection_state;   /* state of the connection (established, etc.) */
    tcp_seq initial_sequence_num;


    tcp_seq next_seq;
    tcp_seq next_ack;
    tcp_seq last_seq;
 
    uint16_t rwnd;
    uint16_t swnd;
    uint16_t cwnd;
    /* any other connection-wide global variables go here */
} context_t;

typedef struct
{
    STCPHeader header;
    char data[MSS];
} Datagram;


static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);


/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is clo
sed.
 */

// void Datagram_Calloc(Datagram *datagram,size_t size)
// {
//     datagram = (Datagram*)calloc(1, size);
// }

bool_t is_SYN(uint8_t flags)
{   
    if (flags >> 1 & 1) return TRUE;
    return FALSE;
}

bool_t is_ACK(uint8_t flags)
{
    if(flags >> 4 & 1) return TRUE;
    return FALSE;
}

bool_t is_FIN(uint8_t flags)
{
    if(flags & 1) return TRUE;
    return FALSE; 
}

void all_free(int num, Datagram **datagram, ...)
{
    va_list valist;
    va_start(valist, *datagram);
    Datagram **copy;
    copy = datagram;
    for (int i = 0; i < num; ++i) {
        free(*copy);
        copy = va_arg(valist, Datagram**);
    }
    va_end(valist);
}

void all_calloc(int num, Datagram **datagram, ...)
{
    va_list vp;
    va_start(vp, *datagram);
    Datagram **copy;
    copy = datagram;
    for (int i = 0; i < num; ++i) {
        *copy = (Datagram*)calloc(1,sizeof(Datagram));
        copy = va_arg(vp,Datagram **);
    }
    va_end(vp);
}

void transport_init(mysocket_t sd, bool_t is_active)
{
    context_t *ctx;

    ctx = (context_t *) calloc(1, sizeof(context_t));
    assert(ctx);

    generate_initial_seq_num(ctx);
    ctx->connection_state = CSTATE_LISTEN;
    ctx->next_seq = ctx->initial_sequence_num;
    
    if (is_active) {
        // client
        Datagram *SYN_Send,*SYNACK_Recv,*ACK_Send;
        size_t DataSize = sizeof(Datagram);
        // too many calloc
        // Datagram_Calloc(SYN_Send,DataSize);
        // Datagram_Calloc(SYNACK_Recv,DataSize);
        // Datagram_Calloc(ACK_Send,DataSize);
        all_calloc(3, &SYN_Send, &SYNACK_Recv, &ACK_Send);
        
        SYN_Send->header.th_seq = htonl(ctx->initial_sequence_num);
        SYN_Send->header.th_flags |= TH_SYN;
        SYN_Send->header.th_win = htonl(ctx->rwnd);
        // ctx->initial_sequence_num = ctx->next_seq;
        // SYN_Send->header.th_seq = htonl(ctx->next_seq);

        stcp_network_send(sd, SYN_Send, DataSize, NULL);
        // fprintf(stderr,"client:SYN send!\n");
        ctx->connection_state = CSTATE_SYN_SENT;
        while(1) {
            stcp_wait_for_event(sd, 2, NULL);
            stcp_network_recv(sd, SYNACK_Recv, DataSize);
            ctx->swnd = MIN(ctx->swnd, ntohl(SYNACK_Recv->header.th_win));
            
            // fprintf(stderr,"client: SYNACK_Recv recv!\n");
            /*
            if (is_SYN(SYNACK_Recv->header.th_flags)) fprintf(stderr,"client:is SYN\n");
            if (is_ACK(SYNACK_Recv->header.th_flags)) fprintf(stderr,"client:is ACK\n");
            if (ntohl(SYNACK_Recv->header.th_ack) == ntohl(SYN_Send->header.th_seq) + 1) fprintf(stderr,"client:ack == seq+1\n");
            */
            if (is_SYN(SYNACK_Recv->header.th_flags) && 
                is_ACK(SYNACK_Recv->header.th_flags) && 
                ntohl(SYNACK_Recv->header.th_ack) == ntohl(SYN_Send->header.th_seq) + 1) {
                    // fprintf(stderr,"client: SYN & ACK recv !\n");
                    ctx->next_ack = ntohl(SYNACK_Recv->header.th_seq) + 1;
                    ctx->next_seq = ntohl(SYNACK_Recv->header.th_ack);
                    ACK_Send->header.th_ack = htonl(ctx->next_ack);
                    ACK_Send->header.th_seq = htonl(ctx->next_seq);
                    ACK_Send->header.th_flags |= TH_ACK;
                    ACK_Send->header.th_win = htonl(ctx->rwnd);
                    stcp_network_send(sd, ACK_Send, DataSize, NULL);
                    ctx->connection_state = CSTATE_ESTABLISHED;
                    // fprintf(stderr,"client: connected!\n");
                    break;
            }
        }
        all_free(3, &SYN_Send, &SYNACK_Recv, &ACK_Send);
        // if(SYN_Send == NULL) fprintf(stderr,"client: SYN_Send free!\n");
        // else fprintf(stderr,"client: SYN_Send do not free!\n");
    } else {
        // server
        Datagram *SYN_Recv, *SYNACK_Send, *ACK_Recv;
        all_calloc(3, &SYN_Recv, &SYNACK_Send, &ACK_Recv);
        while(1) {
            stcp_wait_for_event(sd, 2, NULL);
            stcp_network_recv(sd, SYN_Recv, sizeof(Datagram));
            
            if(is_SYN(SYN_Recv->header.th_flags)) {
                ctx->swnd = MIN(ctx->swnd, ntohl(SYN_Recv->header.th_win));
                ctx->next_ack = ntohl(SYN_Recv->header.th_seq) + 1;
                SYNACK_Send->header.th_seq = htonl(ctx->initial_sequence_num);
                // INC_SEQ(ctx);
                SYNACK_Send->header.th_ack = htonl(ctx->next_ack);
                SYNACK_Send->header.th_flags |= TH_SYN;
                SYNACK_Send->header.th_flags |= TH_ACK;
                SYNACK_Send->header.th_win = htonl(ctx->rwnd);

                stcp_network_send(sd, SYNACK_Send, sizeof(Datagram), NULL);
                ctx->connection_state = CSTATE_SYN_RCVD;
                // fprintf(stderr,"server: SYN from client recived, SYN & ACK sent \n");
                break;
            }
        }
        while(1) {
            stcp_wait_for_event(sd, 2, NULL);
            stcp_network_recv(sd, ACK_Recv, sizeof(Datagram));

            if (is_ACK(ACK_Recv->header.th_flags) && 
                ntohl(ACK_Recv->header.th_ack) == ntohl(SYNACK_Send->header.th_seq) + 1) {
                    
                    ctx->swnd = MIN(ctx->swnd, ntohl(ACK_Recv->header.th_win));
                    ctx->next_ack = ntohl(ACK_Recv->header.th_seq) + 1;
                    ctx->next_seq = ntohl(ACK_Recv->header.th_ack);
                    ctx->connection_state = CSTATE_ESTABLISHED;
                    // fprintf(stderr,"server :connected!\n");
                    break;
                }
        }
        all_free(3, &SYN_Recv, &SYNACK_Send, &ACK_Recv);
        // free(SYN_Recv);
        // if (SYN_Recv==NULL) fprintf(stderr,"server: SYN_Recv free\n");
        // else fprintf(stderr,"server: SYN_Recv do not free!\n");
    }
    /* XXX: you should send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */
    ctx->connection_state = CSTATE_ESTABLISHED;
    stcp_unblock_application(sd);
	// fprintf(stderr,"enter control_loop\n");
    control_loop(sd, ctx);

    /* do any cleanup here */
    free(ctx);
}


/* generate random initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);

#ifdef FIXED_INITNUM
    /* please don't change this! */
    ctx->initial_sequence_num = 1;
#else
    /* you have to fill this up */
    /*ctx->initial_sequence_num =;*/
    ctx->initial_sequence_num = rand() % 256;
#endif
}


/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx)
{
    assert(ctx);


    while (!ctx->done)
    {
        unsigned int event;

        /* see stcp_api.h or stcp_api.c for details of this function */
        /* XXX: you will need to change some of these arguments! */
        event = stcp_wait_for_event(sd, ANY_EVENT, NULL);
        // event->the flags of stcp_wait_for_event()
        /* check whether it was the network, app, or a close request */
        // fprintf(stderr,"event: %d\n",event);
        if (event & APP_DATA)
        {
            /* the application has requested that data be sent */
            /* see stcp_app_recv() */
            Datagram *send_datagram;
            send_datagram = (Datagram*)calloc(1,sizeof(Datagram));
            stcp_app_recv(sd, send_datagram->data, MSS);
            send_datagram->header.th_win = htonl(ctx->swnd);
            send_datagram->header.th_seq = htonl(ctx->next_seq);
            send_datagram->header.th_ack = htonl(ctx->next_ack);
            // INC_SEQ(ctx);
            send_datagram->header.th_flags |= TH_ACK;
            stcp_network_send(sd, send_datagram, sizeof(Datagram), NULL);
            free(send_datagram);
            // fprintf(stderr,"app data send!\n");
        }else if (event & NETWORK_DATA) {
            //recive data from peer
            Datagram *recv_datagram, *sendACK_datagram;
            all_calloc(2, &recv_datagram, &sendACK_datagram);
            recv_datagram = (Datagram*)calloc(1,sizeof(Datagram));

            // fprintf(stderr,"NETWORK_DATA entry\n");
            stcp_network_recv(sd, recv_datagram, sizeof(Datagram));
            if (ctx->next_ack + 3072 - 1 < htonl(recv_datagram->header.th_seq)) {
                fprintf(stderr,"next ack: %d\n",ctx->next_ack);
                fprintf(stderr,"seq: %d\n",recv_datagram->header.th_seq);
                continue;
            }
            
            // fprintf(stderr,"stcp_network_recv ok\n");
            // fprintf(stderr,"th flags: %d\n",recv_datagram->header.th_flags);
            // fprintf(stderr,"connection state: %d\n",ctx->connection_state);
            // ESTAVLISHED -> CLOSE_WAIT server
            // if (is_FIN(recv_datagram->header.th_flags))
                // fprintf(stderr,"server: is fin\n");
            if (is_FIN(recv_datagram->header.th_flags) &&
                ctx->connection_state == CSTATE_ESTABLISHED) {
                ctx->next_ack = ntohl(recv_datagram->header.th_seq) + 1;
                ctx->next_seq = ntohl(recv_datagram->header.th_ack);
                ctx->swnd = MIN(ctx->swnd, ntohl(recv_datagram->header.th_win));
                // fprintf(stderr,"ack flags: %d\n",sendACK_datagram->header.th_flags);
                sendACK_datagram->header.th_flags |= TH_ACK;
                sendACK_datagram->header.th_seq = htonl(ctx->next_seq);
                // INC_SEQ(ctx);
                sendACK_datagram->header.th_win = htonl(ctx->rwnd);
                sendACK_datagram->header.th_ack = htonl(ctx->next_ack);
                // fprintf(stderr,"ack flags: %d\n",sendACK_datagram->header.th_flags);
                stcp_network_send(sd, sendACK_datagram, sizeof(Datagram), NULL);
                stcp_fin_received(sd);
                ctx->connection_state = CSTATE_CLOSE_WAIT;
                // fprintf(stderr,"ESTAVLISHED -> CLOSE_WAIT server\n");
            }
 
            // FIN_WAIT_1 -> FIN_WAIT_2 server
            else if (is_ACK(recv_datagram->header.th_flags) &&
                        ctx->connection_state == CSTATE_FIN_WAIT_1) {
                ctx->next_ack = ntohl(recv_datagram->header.th_seq) + 1;
                ctx->next_seq = ntohl(recv_datagram->header.th_ack);
                ctx->swnd = MIN(ctx->swnd, ntohl(recv_datagram->header.th_win));
                ctx->connection_state = CSTATE_FIN_WAIT_2;
                // fprintf(stderr,"FIN_WAIT_1 -> FIN_WAIT_2 client\n");
            } 
            // FIN_WAIT_2 -> TIME_WAIT -> CLOSED server
            else if (is_FIN(recv_datagram->header.th_flags) &&
                        ctx->connection_state == CSTATE_FIN_WAIT_2) {
                ctx->next_ack = ntohl(recv_datagram->header.th_seq) + 1;
                ctx->next_seq = ntohl(recv_datagram->header.th_ack);
                ctx->swnd = MIN(ctx->swnd, ntohl(recv_datagram->header.th_win));
                sendACK_datagram->header.th_flags |= TH_ACK;
                sendACK_datagram->header.th_ack = htonl(ctx->next_ack);
                sendACK_datagram->header.th_seq = htonl(ctx->next_seq);
                sendACK_datagram->header.th_win = htonl(ctx->rwnd);
                // INC_SEQ(ctx);
                
                stcp_network_send(sd, sendACK_datagram, sizeof(Datagram), NULL);
                stcp_fin_received(sd);
                ctx->connection_state = CSTATE_TIME_WAIT;
                /* wait */
                ctx->connection_state = CSTATE_CLOSED;
                // fprintf(stderr,"FIN_WAIT_2 -> TIME_WAIT -> CLOSED client\n");
                ctx->done = TRUE;
                // fprintf(stderr,"client: ctx done\n");
                
            }
            //LASR_ACK -> CLOSED client
            else if (is_ACK(recv_datagram->header.th_flags) && 
                        ctx->connection_state == CSTATE_LAST_ACK) {
                ctx->next_ack = ntohl(recv_datagram->header.th_seq) + 1;
                ctx->next_seq = ntohl(recv_datagram->header.th_ack);
                ctx->swnd = MIN(ctx->swnd, ntohl(recv_datagram->header.th_win));
                ctx->connection_state = CSTATE_CLOSED;
                // fprintf(stderr,"LASR_ACK -> CLOSED client\n");
                ctx->done = TRUE;
                // fprintf(stderr,"client: ctx done\n");
            } else {
                if (ctx->next_ack + 3072 -1 < htonl(recv_datagram->header.th_seq) + MSS) {
                    Datagram *temp_1,*temp_2;
                    all_calloc(2, &temp_1, &temp_2);
                    temp_1->header = recv_datagram->header;
                    memcpy(temp_1->data,recv_datagram->data,sizeof(char)*(ctx->next_ack + 3071 - htonl(recv_datagram->header.th_seq)));
                    stcp_app_send(sd, temp_1->data, MIN(MSS, strlen(temp_1->data)));
                    temp_2->header = recv_datagram->header;
                    memcpy(temp_2->data,recv_datagram->data+(ctx->next_ack + 3071 - htonl(recv_datagram->header.th_seq)),sizeof(char)*(htonl(recv_datagram->header.th_seq) + MSS - (ctx->next_ack + 3071)));
                    stcp_app_send(sd, temp_2->data, MIN(MSS, strlen(temp_2->data)));
                }else{
                    stcp_app_send(sd, recv_datagram->data, MIN(MSS, strlen(recv_datagram->data)));
                }
                ctx->next_ack = htonl(recv_datagram->header.th_seq) + MSS;
                // fprintf(stderr,"app send!\n");
                // fprintf(stderr,"data: %s\n",recv_datagram->data);
            }
            all_free(2, &recv_datagram, &sendACK_datagram);
        }else if(event & APP_CLOSE_REQUESTED) {
            // fprintf(stderr,"app close requested\n");
            Datagram *sendFIN_datagram;
            all_calloc(1,&sendFIN_datagram);
            // fprintf(stderr,"connection_state: %d\n",ctx->connection_state);
            if (ctx->connection_state == CSTATE_CLOSE_WAIT) {
                sendFIN_datagram->header.th_flags |= TH_FIN;
                sendFIN_datagram->header.th_win = htonl(ctx->swnd);
                sendFIN_datagram->header.th_seq = htonl(ctx->next_seq);
                sendFIN_datagram->header.th_ack = htonl(ctx->next_ack);
                // INC_SEQ(ctx);
                stcp_network_send(sd, sendFIN_datagram, sizeof(Datagram),NULL);
                ctx->connection_state = CSTATE_LAST_ACK;
                // fprintf(stderr,"CLOSE_WAIT -> LAST_ACK server\n");
            } else if (ctx->connection_state == CSTATE_ESTABLISHED) {
                // fprintf(stderr,"enter case\n");
                // fprintf(stderr,"ESTABLISHED -> FIN_WAIT_1 client\n");
                // fprintf(stderr,"th_flags:%d\n",sendFIN_datagram->header.th_flags);
                sendFIN_datagram->header.th_flags |= TH_FIN;
                sendFIN_datagram->header.th_win = htonl(ctx->swnd);
                sendFIN_datagram->header.th_seq = htonl(ctx->next_seq);
                sendFIN_datagram->header.th_ack = htonl(ctx->next_ack);
                // INC_SEQ(ctx);
                // fprintf(stderr,"th_flags:%d\n",sendFIN_datagram->header.th_flags);
                // fprintf(stderr,"set flag fin ok\n");
                stcp_network_send(sd, sendFIN_datagram, sizeof(Datagram),NULL);
                // fprintf(stderr,"net work send ok\n");
                ctx->connection_state = CSTATE_FIN_WAIT_1;
                // fprintf(stderr,"ESTABLISHED -> FIN_WAIT_1 client\n");
            }
            free(sendFIN_datagram);
        }

        /* etc. */
    }
}


/**********************************************************************/
/* our_dprintf
 *
 * Send a formatted message to stdout.
 * 
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void our_dprintf(const char *format,...)
{
    va_list argptr;
    char buffer[1024];

    assert(format);
    va_start(argptr, format);
    vsnprintf(buffer, sizeof(buffer), format, argptr);
    va_end(argptr);
    fputs(buffer, stdout);
    fflush(stdout);
}



