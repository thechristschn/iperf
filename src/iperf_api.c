
/*
 * Copyright (c) 2009, The Regents of the University of California, through
 * Lawrence Berkeley National Laboratory (subject to receipt of any required
 * approvals from the U.S. Dept. of Energy).  All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_udp.h"
#include "iperf_tcp.h"
#include "timer.h"
#include "net.h"
#include "units.h"
#include "tcp_window_size.h"
#include "uuid.h"
#include "locale.h"

jmp_buf env;			/* to handle longjmp on signal */

/*************************************************************/

/*
 * check to see if client has sent the requested number of bytes to the
 * server yet
 */

/*
 * XXX: should probably just compute this as we go and store it in the
 * iperf_test structure -blt
 */
int
all_data_sent(struct iperf_test * test)
{
    if (test->default_settings->bytes == 0) {
        return 0;
    } else {
        uint64_t  total_bytes = 0;
        struct iperf_stream *sp;

        sp = test->streams;

        while (sp) {
            total_bytes += sp->result->bytes_sent;
            sp = sp->next;
        }

        if (total_bytes >= (test->num_streams * test->default_settings->bytes)) {
            return 1;
        } else {
            return 0;
        }
    }

}

int
iperf_send(struct iperf_test *test)
{
    int result;
    char *prot;
    fd_set temp_write_set;
    struct timeval tv;
    int64_t delayus, adjustus, dtargus;
    struct iperf_stream *sp;
    static struct timer *timer, *stats_interval, *reporter_interval;

    if (test->state == TEST_START) {
        timer = stats_interval = reporter_interval = NULL;

        // Not sure what the following code does yet. It's UDP, so I'll get to fixing it eventually
        if (test->protocol == Pudp) {
            prot = "UDP";
            dtargus = (int64_t) (test->default_settings->blksize) * SEC_TO_US * 8;
            dtargus /= test->default_settings->rate;

            assert(dtargus != 0);

            delayus = dtargus;
            adjustus = 0;

            printf("iperf_run_client: adjustus: %lld, delayus %lld \n", adjustus, delayus);

            for (sp = test->streams; sp != NULL; sp = sp->next)
                sp->send_timer = new_timer(0, dtargus);

        } else {
            prot = "TCP";
        }

        /* if -n specified, set zero timer */
        // Set timers and print usage message
        if (test->default_settings->bytes == 0) {
            timer = new_timer(test->duration, 0);
            printf("Starting Test: protocol: %s, %d streams, %d byte blocks, %d second test \n",
                prot, test->num_streams, test->default_settings->blksize, test->duration);
        } else {
            timer = new_timer(0, 0);
            printf("Starting Test: protocol: %s, %d streams, %d byte blocks, %d bytes to send\n",
                prot, test->num_streams, test->default_settings->blksize, (int) test->default_settings->bytes);
        }
        if (test->stats_interval != 0)
            stats_interval = new_timer(test->stats_interval, 0);
        if (test->reporter_interval != 0)
            reporter_interval = new_timer(test->reporter_interval, 0);
        
        test->state = TEST_RUNNING;
        if (write(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
            perror("write TEST_RUNNING");
            exit(1);
        }

    } else if (test->state == TEST_RUNNING) {
        memcpy(&temp_write_set, &test->write_set, sizeof(fd_set));
        tv.tv_sec = 15;
        tv.tv_usec = 0;

        result = select(test->max_fd + 1, NULL, &temp_write_set, NULL, &tv);
        if (result < 0 && errno != EINTR) {
            perror("select iperf_send");
            return -1;
        }
        if (result > 0) {
            if (!all_data_sent(test) && !timer->expired(timer)) {
                for (sp = test->streams; sp != NULL; sp = sp->next) {
                    if (FD_ISSET(sp->socket, &temp_write_set)) {
                        if (sp->snd(sp) < 0) {
                            // XXX: Do better error handling
                            perror("iperf_client_start: snd");
                        }
                        FD_CLR(sp->socket, &temp_write_set);
                    }
                }

                // Perform callbacks
                if ((test->stats_interval != 0) && stats_interval->expired(stats_interval)) {
                    test->stats_callback(test);
                    update_timer(stats_interval, test->stats_interval, 0);
                }
                if ((test->reporter_interval != 0) && reporter_interval->expired(reporter_interval)) {
                    test->reporter_callback(test);
                    update_timer(reporter_interval, test->reporter_interval, 0);
                }
            } else {
                free(timer);
                free(stats_interval);
                free(reporter_interval);

                // Send TEST_DONE (ALL_STREAMS_END) message
                test->state = TEST_END;
                if (write(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
                    perror("write TEST_END");
                    return -1;
                }
            }
        }
    } 

    return 0;
}

int
iperf_recv(struct iperf_test *test)
{
    int result;
    fd_set temp_read_set;
    struct timeval tv;
    struct iperf_stream *sp;

    if (test->state == TEST_RUNNING) {
        memcpy(&temp_read_set, &test->read_set, sizeof(fd_set));
        tv.tv_sec = 15;
        tv.tv_usec = 0;

        result = select(test->max_fd + 1, &temp_read_set, NULL, NULL, &tv);
        if (result < 0) {
            perror("select iperf_recv");
            return -1;
        }
        if (result > 0) {
            for (sp = test->streams; sp != NULL; sp = sp->next) {
                if (FD_ISSET(sp->socket, &temp_read_set)) {
                    if (sp->rcv(sp) < 0) {
                        // XXX: Do better error handling
                        perror("sp->rcv(sp)");
                    }
                    FD_CLR(sp->socket, &temp_read_set);
                }
            }
        }
    }

    return 0;
}


/*********************************************************/

int
package_parameters(struct iperf_test *test)
{
    char pstring[256];
    char optbuf[128];
    memset(pstring, 0, 256*sizeof(char));

    *pstring = ' ';

    if (test->protocol == Ptcp) {
        strcat(pstring, "-p ");
    } else if (test->protocol == Pudp) {
        strcat(pstring, "-u ");
    }

    sprintf(optbuf, "-P %d ", test->num_streams);
    strcat(pstring, optbuf);
    
    if (test->reverse)
        strcat(pstring, "-R ");
    
    if (test->default_settings->socket_bufsize) {
        sprintf(optbuf, "-w %d ", test->default_settings->socket_bufsize);
        strcat(pstring, optbuf);
    }

    if (test->default_settings->unit_format) {
        sprintf(optbuf, "-f %c ", test->default_settings->unit_format);
        strcat(pstring, optbuf);
    }

    if (strcmp(test->default_settings->cookie, "") != 0) {
        sprintf(optbuf, "-C %s ", test->default_settings->cookie);
        strcat(pstring, optbuf);
    }

    *pstring = (char) (strlen(pstring) - 1);

    if (write(test->ctrl_sck, pstring, (size_t) strlen(pstring)) < 0) {
        perror("write pstring");
        return -1;
    }

    return 0;
}


int
parse_parameters(struct iperf_test *test)
{
    int n;
    char *arg, **args;
    char len, ch;
    char pstring[256];
    char readbuf[256];

    memset(pstring, 0, 256 * sizeof(char));

    if (read(test->ctrl_sck, &len, sizeof(char)) < 0) {
        perror("read len");
        return -1;
    }

    while (len > 0) {
        memset(readbuf, 0, 256 * sizeof(char));
        if ((len -= read(test->ctrl_sck, readbuf, (size_t) len)) < 0) {
            perror("read pstring");
            return -1;
        }
        strcat(pstring, readbuf);
    }

    for (arg = strtok(pstring, " "), n = 0, args = NULL; arg; arg = strtok(NULL, " ")) {
        if ((args = realloc(args, (n+1)*sizeof(char *))) == NULL) {
            perror("realloc");
            return -1;
        }
        args[n] = arg;
        n++;
    }

    while ((ch = getopt(n, args, "puP:Rw:f:C:")) != -1) {
        switch (ch) {
            case 'p':
                test->protocol = Ptcp;
                break;
            case 'u':
                test->protocol = Pudp;
                break;
            case 'P':
                test->num_streams = atoi(optarg);
                break;
            case 'R':
                test->reverse = 1;
                break;
            case 'w':
                test->default_settings->socket_bufsize = atoi(optarg);
                break;
            case 'f':
                test->default_settings->unit_format = *optarg;
                break;
            case 'C':
                memset(test->default_settings->cookie, 0, COOKIE_SIZE);
                memcpy(test->default_settings->cookie, optarg, COOKIE_SIZE);
                break;
        }
    }

    free(args);

    return 0;
}

/**
 * iperf_exchange_parameters - handles the param_Exchange part for client
 *
 */

int
iperf_exchange_parameters(struct iperf_test * test)
{
    if (test->role == 'c') {

        // XXX: Probably should get the cookie at the start of iperf client rather than
        //      waiting till here
        get_uuid(test->default_settings->cookie);

        package_parameters(test);

    } else {

        parse_parameters(test);

        printf("test cookie: %s\n", test->default_settings->cookie);

        test->prot_listener = netannounce(test->protocol, NULL, test->server_port + 1);
        FD_SET(test->prot_listener, &test->read_set);
        FD_SET(test->prot_listener, &test->write_set);
        test->max_fd = (test->prot_listener > test->max_fd) ? test->prot_listener : test->max_fd;

        // Send the control message to create streams and start the test
        test->state = CREATE_STREAMS;
        if (write(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
            perror("write CREATE_STREAMS");
            return -1;
        }

    }

    return 0;
}

/*************************************************************/
/**
 * add_to_interval_list -- adds new interval to the interval_list
 *
 */

void
add_to_interval_list(struct iperf_stream_result * rp, struct iperf_interval_results * new)
{
    struct iperf_interval_results *ip = NULL;

    ip = (struct iperf_interval_results *) malloc(sizeof(struct iperf_interval_results));
    memcpy(ip, new, sizeof(struct iperf_interval_results));
    ip->next = NULL;

    if (rp->interval_results == NULL) {	/* if 1st interval */
        rp->interval_results = ip;
        rp->last_interval_results = ip; /* pointer to last element in list */
    } else { /* add to end of list */
        rp->last_interval_results->next = ip;
        rp->last_interval_results = ip;
    }
}

 /*************************************************************/
 /* for debugging only */
void
display_interval_list(struct iperf_stream_result * rp, int tflag)
{
    struct iperf_interval_results *n;
    float gb = 0.;

    n = rp->interval_results;

    printf("----------------------------------------\n");
    while (n != NULL) {
        gb = (float) n->bytes_transferred / (1024. * 1024. * 1024.);
        printf("Interval = %f\tGBytes transferred = %.3f\n", n->interval_duration, gb);
        if (tflag)
            print_tcpinfo(n);
        n = n->next;
    }
}

/************************************************************/

/**
 * receive_result_from_server - Receives result from server
 */

void
receive_result_from_server(struct iperf_test * test)
{
    int result;
    struct iperf_stream *sp;
    int size = 0;
    char *buf = NULL;

    printf("in receive_result_from_server \n");
    sp = test->streams;
    size = MAX_RESULT_STRING;

    buf = (char *) malloc(size);

    printf("receive_result_from_server: send ALL_STREAMS_END to server \n");
    sp->settings->state = ALL_STREAMS_END;
    sp->snd(sp);		/* send message to server */

    printf("receive_result_from_server: send RESULT_REQUEST to server \n");
    sp->settings->state = RESULT_REQUEST;
    sp->snd(sp);		/* send message to server */

    /* receive from server */

    printf("reading results (size=%d) back from server \n", size);
    do {
        result = recv(sp->socket, buf, size, 0);
    } while (result == -1 && errno == EINTR);
    printf("Got size of results from server: %d \n", result);

    printf(server_reporting, sp->socket);
    puts(buf);			/* prints results */
    free(buf);

}

/*************************************************************/

/**
 * connect_msg -- displays connection message
 * denoting sender/receiver details
 *
 */

void
connect_msg(struct iperf_stream * sp)
{
    char ipl[512], ipr[512];

    inet_ntop(AF_INET, (void *) (&((struct sockaddr_in *) & sp->local_addr)->sin_addr), (void *) ipl, sizeof(ipl));
    inet_ntop(AF_INET, (void *) (&((struct sockaddr_in *) & sp->remote_addr)->sin_addr), (void *) ipr, sizeof(ipr));

    printf("[%3d] local %s port %d connected to %s port %d\n",
        sp->socket,
        ipl, ntohs(((struct sockaddr_in *) & sp->local_addr)->sin_port),
        ipr, ntohs(((struct sockaddr_in *) & sp->remote_addr)->sin_port));
}

/*************************************************************/
/**
 * Display -- Displays results for test
 * Mainly for DEBUG purpose
 *
 */

void
Display(struct iperf_test * test)
{
    int count = 1;
    struct iperf_stream *n;

    n = test->streams;

    printf("===============DISPLAY==================\n");

    while (n != NULL) {
        if (test->role == 'c') {
            printf("position-%d\tsp=%llu\tsocket=%d\tMbytes sent=%u\n",
                count++, (uint64_t) n, n->socket, (unsigned int) (n->result->bytes_sent / (float) MB));
        } else {
            printf("position-%d\tsp=%llu\tsocket=%d\tMbytes received=%u\n",
                count++, (uint64_t) n, n->socket, (unsigned int) (n->result->bytes_received / (float) MB));
        }
        n = n->next;
    }
    printf("=================END====================\n");
    fflush(stdout);
}

/**************************************************************************/

struct iperf_test *
iperf_new_test()
{
    struct iperf_test *testp;

    testp = (struct iperf_test *) malloc(sizeof(struct iperf_test));
    if (!testp) {
        perror("malloc");
        return (NULL);
    }
    /* initialise everything to zero */
    memset(testp, 0, sizeof(struct iperf_test));

    testp->default_settings = (struct iperf_settings *) malloc(sizeof(struct iperf_settings));
    memset(testp->default_settings, 0, sizeof(struct iperf_settings));

    /* return an empty iperf_test* with memory alloted. */
    return testp;
}

/**************************************************************************/
void
iperf_defaults(struct iperf_test * testp)
{
    testp->protocol = Ptcp;
    testp->role = 's';
    testp->duration = DURATION;
    testp->server_port = PORT;
    testp->ctrl_sck = -1;

    testp->new_stream = iperf_new_tcp_stream;
    testp->stats_callback = iperf_stats_callback;
    testp->reporter_callback = iperf_reporter_callback;

    testp->stats_interval = 0;
    testp->reporter_interval = 0;
    testp->num_streams = 1;

    testp->default_settings->unit_format = 'a';
    testp->default_settings->socket_bufsize = 0;	/* use autotuning */
    testp->default_settings->blksize = DEFAULT_TCP_BLKSIZE;
    testp->default_settings->rate = RATE;	/* UDP only */
    testp->default_settings->state = TEST_START;
    testp->default_settings->mss = 0;
    testp->default_settings->bytes = 0;
    memset(testp->default_settings->cookie, '\0', COOKIE_SIZE);
}

/**************************************************************************/

int
iperf_create_streams(struct iperf_test *test)
{
    struct iperf_stream *sp;
    int i, s;

    for (i = 0; i < test->num_streams; ++i) {
        s = netdial(test->protocol, test->server_hostname, test->server_port + 1);
        if (s < 0) {
            perror("netdial stream");
            return -1;
        }
        // printf("File descriptor for stream %d: %d\n", i, s);
        FD_SET(s, &test->read_set);
        FD_SET(s, &test->write_set);
        test->max_fd = (test->max_fd < s) ? s : test->max_fd;

        // XXX: This doesn't fit our API model!
        sp = test->new_stream(test);
        sp->socket = s;
        iperf_init_stream(sp, test);
        iperf_add_stream(test, sp);

        connect_msg(sp);
    }

    return 0;
}

int
iperf_handle_message_client(struct iperf_test *test)
{
    if (read(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
        // indicate error on read
        return -1;
    }

    switch (test->state) {
        case CREATE_STREAMS:
            iperf_create_streams(test);
            break;
        case TEST_START:
            break;
        case TEST_RUNNING:
            break;
        case TEST_END:
            break;
        case DISPLAY_RESULTS:
            iperf_client_end(test);
            break;
        case IPERF_DONE:
            break;
        case SERVER_TERMINATE:
            fprintf(stderr, "The server has terminated. Exiting...\n");
            exit(1);
        case ACCESS_DENIED:
            fprintf(stderr, "The server is busy running a test. Try again later.\n");
            exit(0);
        default:
            // XXX: This needs to be replaced
            printf("How did you get here? test->state = %d\n", test->state);
            break;
    }

    return 0;
}

/* iperf_connect -- client to server connection function */
int
iperf_connect(struct iperf_test *test)
{
    printf("Connecting to host %s, port %d\n", test->server_hostname, test->server_port);

    FD_ZERO(&test->read_set);
    FD_ZERO(&test->write_set);

    /* Create and connect the control channel */
    test->ctrl_sck = netdial(test->protocol, test->server_hostname, test->server_port);
    if (test->ctrl_sck < 0) {
        return -1;
    }

    FD_SET(test->ctrl_sck, &test->read_set);
    FD_SET(test->ctrl_sck, &test->write_set);
    test->max_fd = (test->ctrl_sck > test->max_fd) ? test->ctrl_sck : test->max_fd;

    /* Exchange parameters */
    test->state = PARAM_EXCHANGE;
    if (write(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
        perror("write PARAM_EXCHANGE");
        return -1;
    }
    if (iperf_exchange_parameters(test) < 0) {
        fprintf(stderr, "iperf_exchange_parameters failed\n");
        return -1;
    }

    return 0;
}

/**************************************************************************/
void
iperf_free_test(struct iperf_test * test)
{
    free(test->default_settings);

    // This funciton needs to be updated to free and close streams
    // Currently it just sets the pointer to the streams list to NULL...

    close(test->listener_sock_tcp);
    close(test->listener_sock_udp);

    test->streams = NULL;
    test->accept = NULL;
    test->stats_callback = NULL;
    test->reporter_callback = NULL;
    test->new_stream = NULL;
    free(test);
}

/**************************************************************************/

/**
 * iperf_stats_callback -- handles the statistic gathering for both the client and server
 *
 * XXX: This function needs to be updated to reflect the new code
 */


void
iperf_stats_callback(struct iperf_test * test)
{
    struct iperf_stream *sp;
    struct iperf_stream_result *rp = NULL;
    struct iperf_interval_results *ip = NULL, temp;

    for (sp = test->streams; sp != NULL; sp = sp->next) {
        rp = sp->result;

        if (test->role == 'c')
            temp.bytes_transferred = rp->bytes_sent_this_interval;
        else
            temp.bytes_transferred = rp->bytes_received_this_interval;
     
        ip = sp->result->interval_results;
        /* result->end_time contains timestamp of previous interval */
        if ( ip != NULL ) /* not the 1st interval */
            memcpy(&temp.interval_start_time, &sp->result->end_time, sizeof(struct timeval));
        else /* or use timestamp from beginning */
            memcpy(&temp.interval_start_time, &sp->result->start_time, sizeof(struct timeval));
        /* now save time of end of this interval */
        gettimeofday(&sp->result->end_time, NULL);
        memcpy(&temp.interval_end_time, &sp->result->end_time, sizeof(struct timeval));
        temp.interval_duration = timeval_diff(&temp.interval_start_time, &temp.interval_end_time);
        //temp.interval_duration = timeval_diff(&temp.interval_start_time, &temp.interval_end_time);
        if (test->tcp_info)
            get_tcpinfo(test, &temp);
        //printf(" iperf_stats_callback: adding to interval list: \n");
        add_to_interval_list(rp, &temp);
        rp->bytes_sent_this_interval = rp->bytes_received_this_interval = 0;

    }

}

/**************************************************************************/

/**
 * iperf_reporter_callback -- handles the report printing
 *
 *returns report
 * XXX: This function needs to be updated to reflect the new code
 */

void
iperf_reporter_callback(struct iperf_test * test)
{
    int total_packets = 0, lost_packets = 0;
    char ubuf[UNIT_LEN];
    char nbuf[UNIT_LEN];
    struct iperf_stream *sp = NULL;
    iperf_size_t bytes = 0, total_bytes = 0;
    double start_time, end_time;
    struct iperf_interval_results *ip = NULL;

    switch (test->state) {
        case TEST_RUNNING:
        case STREAM_RUNNING:
            /* print interval results for each stream */
            for (sp = test->streams; sp != NULL; sp = sp->next) {
                print_interval_results(test, sp);
                bytes += sp->result->interval_results->bytes_transferred; /* sum up all streams */
            }
            if (bytes <=0 ) { /* this can happen if timer goes off just when client exits */
                fprintf(stderr, "error: bytes <= 0!\n");
                break;
            }
            /* next build string with sum of all streams */
            if (test->num_streams > 1) {
                sp = test->streams; /* reset back to 1st stream */
                ip = test->streams->result->last_interval_results;	/* use 1st stream for timing info */

                unit_snprintf(ubuf, UNIT_LEN, (double) (bytes), 'A');
                unit_snprintf(nbuf, UNIT_LEN, (double) (bytes / ip->interval_duration),
                        test->default_settings->unit_format);

                start_time = timeval_diff(&sp->result->start_time,&ip->interval_start_time);
                end_time = timeval_diff(&sp->result->start_time,&ip->interval_end_time);
                printf(report_sum_bw_format, start_time, end_time, ubuf, nbuf);

#if defined(linux) || defined(__FreeBSD__)			/* is it usful to figure out a way so sum * TCP_info acrross multiple streams? */
                if (test->tcp_info)
                    print_tcpinfo(ip);
#endif
            }
            break;
        case TEST_END:
        case DISPLAY_RESULTS:
            /* print final summary for all intervals */
            start_time = 0.;
            sp = test->streams;
            end_time = timeval_diff(&sp->result->start_time, &sp->result->end_time);
            for (sp = test->streams; sp != NULL; sp = sp->next) {
                if (test->role == 'c')
                    bytes = sp->result->bytes_sent;
                else
                    bytes = sp->result->bytes_received;
                total_bytes += bytes;
                if (test->protocol == Pudp) {
                    total_packets += sp->packet_count;
                    lost_packets += sp->cnt_error;
                }
                if (bytes > 0 ) {
                    unit_snprintf(ubuf, UNIT_LEN, (double) (bytes), 'A');
                    unit_snprintf(nbuf, UNIT_LEN, (double) (bytes / end_time), test->default_settings->unit_format);
                    if (test->protocol == Ptcp) { 
                        printf(report_bw_format, sp->socket, start_time, end_time, ubuf, nbuf);

#if defined(linux) || defined(__FreeBSD__)
                        if (test->tcp_info) {
                            ip = sp->result->last_interval_results;	
                            print_tcpinfo(ip);
                        }
#endif
                    } else {
                        printf(report_bw_jitter_loss_format, sp->socket, start_time,
                                end_time, ubuf, nbuf, sp->jitter * 1000, sp->cnt_error, 
                                sp->packet_count, (double) (100.0 * sp->cnt_error / sp->packet_count));
                        if (test->role == 'c') {
                            printf(report_datagrams, sp->socket, sp->packet_count);
                        }
                        if (sp->outoforder_packets > 0)
                            printf(report_sum_outoforder, start_time, end_time, sp->cnt_error);
                    }
                }
            }

            unit_snprintf(ubuf, UNIT_LEN, (double) total_bytes, 'A');
            unit_snprintf(nbuf, UNIT_LEN, (double) total_bytes / end_time, test->default_settings->unit_format);

            if (test->num_streams > 1) {
                if (test->protocol == Ptcp) {
                    printf(report_sum_bw_format, start_time, end_time, ubuf, nbuf);
                } else {
                    printf(report_sum_bw_jitter_loss_format, start_time, end_time, ubuf, nbuf, sp->jitter,
                        lost_packets, total_packets, (double) (100.0 * lost_packets / total_packets));
                }

                if ((test->print_mss != 0) && (test->role == 'c')) {
                    printf("The TCP maximum segment size mss = %d\n", getsock_tcp_mss(sp->socket));
                }
            }
            break;
    } 

}

/**************************************************************************/
void
print_interval_results(struct iperf_test * test, struct iperf_stream * sp)
{
    char ubuf[UNIT_LEN];
    char nbuf[UNIT_LEN];
    double st = 0., et = 0.;
    struct iperf_interval_results *ir = NULL;

    ir = sp->result->last_interval_results; /* get last entry in linked list */
    if (ir == NULL) {
        printf("print_interval_results Error: interval_results = NULL \n");
        return;
    }
    if (sp == test->streams) {
        printf(report_bw_header);
    }

    unit_snprintf(ubuf, UNIT_LEN, (double) (ir->bytes_transferred), 'A');
    unit_snprintf(nbuf, UNIT_LEN, (double) (ir->bytes_transferred / ir->interval_duration),
            test->default_settings->unit_format);
    
    st = timeval_diff(&sp->result->start_time,&ir->interval_start_time);
    et = timeval_diff(&sp->result->start_time,&ir->interval_end_time);
    
    printf(report_bw_format, sp->socket, st, et, ubuf, nbuf);

#if defined(linux) || defined(__FreeBSD__)
    if (test->tcp_info)
        print_tcpinfo(ir);
#endif
}

/**************************************************************************/
void
safe_strcat(char *s1, char *s2)
{
    if (strlen(s1) + strlen(s2) < MAX_RESULT_STRING) {
        strcat(s1, s2);
    } else {
        printf("Error: results string too long \n");
        exit(-1);		/* XXX: should return an error instead! */
                        /* but code that calls this needs to check for error first */
                        //return -1;
    }
}

/**************************************************************************/
void
iperf_free_stream(struct iperf_stream * sp)
{
    /* XXX: need to free interval list too! */
    free(sp->buffer);
    free(sp->settings);
    free(sp->result);
    free(sp->send_timer);
    free(sp);
}

/**************************************************************************/
struct iperf_stream *
iperf_new_stream(struct iperf_test *testp)
{
    int i;
    struct iperf_stream *sp;

    sp = (struct iperf_stream *) malloc(sizeof(struct iperf_stream));
    if (!sp) {
        perror("malloc");
        return (NULL);
    }
    memset(sp, 0, sizeof(struct iperf_stream));

    sp->buffer = (char *) malloc(testp->default_settings->blksize);
    sp->settings = (struct iperf_settings *) malloc(sizeof(struct iperf_settings));
    sp->result = (struct iperf_stream_result *) malloc(sizeof(struct iperf_stream_result));
    
    /* Make a per stream copy of default_settings in each stream structure */
    // XXX: These settings need to be moved to the test struct
    memcpy(sp->settings, testp->default_settings, sizeof(struct iperf_settings));

    /* Randomize the buffer */
    srandom(time(0));
    for (i = 0; i < testp->default_settings->blksize; ++i)
        sp->buffer[i] = random();

    sp->socket = -1;

    // XXX: Not entirely sure what this does
    sp->stream_id = (uint64_t) sp;

    //  XXX: Some of this code is needed, even though everything is already zero.
    sp->packet_count = 0;
    sp->jitter = 0.0;
    sp->prev_transit = 0.0;
    sp->outoforder_packets = 0;
    sp->cnt_error = 0;

    sp->send_timer = NULL;
    sp->next = NULL;

    sp->result->interval_results = NULL;
    sp->result->last_interval_results = NULL;
    sp->result->bytes_received = 0;
    sp->result->bytes_sent = 0;
    sp->result->bytes_received_this_interval = 0;
    sp->result->bytes_sent_this_interval = 0;

    gettimeofday(&sp->result->start_time, NULL);

    sp->settings->state = STREAM_BEGIN;
    return sp;
}

/**************************************************************************/
void
iperf_init_stream(struct iperf_stream * sp, struct iperf_test * testp)
{
    socklen_t len;

    len = sizeof(struct sockaddr_in);

    if (getsockname(sp->socket, (struct sockaddr *) &sp->local_addr, &len) < 0) {
        perror("getsockname");
    }
    if (getpeername(sp->socket, (struct sockaddr *) &sp->remote_addr, &len) < 0) {
        perror("getpeername");
    }
    if (testp->protocol == Ptcp) {
        if (set_tcp_windowsize(sp->socket, testp->default_settings->socket_bufsize,
                testp->role == 's' ? SO_RCVBUF : SO_SNDBUF) < 0)
            fprintf(stderr, "unable to set window size\n");

        /* set TCP_NODELAY and TCP_MAXSEG if requested */
        set_tcp_options(sp->socket, testp->no_delay, testp->default_settings->mss);
    }

}

/**************************************************************************/
int
iperf_add_stream(struct iperf_test * test, struct iperf_stream * sp)
{
    struct iperf_stream *n;

    if (!test->streams) {
        test->streams = sp;
        return 1;
    } else {
        n = test->streams;
        while (n->next)
            n = n->next;
        n->next = sp;
        return 1;
    }

    return 0;
}


/**************************************************************************/

int
iperf_client_end(struct iperf_test *test)
{
    struct iperf_stream *sp;
    printf("Test Complete. Summary Results:\n");
 
    /* show final summary */
    test->stats_callback(test);
    test->reporter_callback(test);

    /* Requesting for result from Server
     *
     * This still needs to be implemented. It looks like the last
     * intern worked on it, but it needs to be finished.
     */

    // XXX: Need to implement getting results from server
    test->state = RESULT_REQUEST;
    //receive_result_from_server(test);	/* XXX: currently broken! */
    //result_string = test->reporter_callback(test);
    //printf("Summary results as measured by the server: \n");
    //puts(result_string);

    //printf("Done getting/printing results. \n");

    /* Deleting all streams - CAN CHANGE FREE_STREAM FN */
    for (sp = test->streams; sp != NULL; sp = sp->next) {
        close(sp->socket);
        iperf_free_stream(sp);
    }

    test->state = IPERF_DONE;
    if (write(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
        perror("write IPERF_DONE");
        return -1;
    }

    return 0;
}

void
sig_handler(int sig)
{
   longjmp(env, 1); 
}

int
iperf_run_client(struct iperf_test * test)
{
    int result;
    fd_set temp_read_set, temp_write_set;
    struct timeval tv;

    /* Start the client and connect to the server */
    if (iperf_connect(test) < 0) {
        // set error and return
        return -1;
    }
    
    signal(SIGINT, sig_handler);
    if (setjmp(env)) {
        fprintf(stderr, "Interrupt received. Exiting...\n");
        test->state = CLIENT_TERMINATE;
        if (write(test->ctrl_sck, &test->state, sizeof(int)) < 0) {
            fprintf(stderr, "Unable to send CLIENT_TERMINATE message to serer\n");
        }
        exit(1);
    }

    while (test->state != IPERF_DONE) {

        memcpy(&temp_read_set, &test->read_set, sizeof(fd_set));
        memcpy(&temp_write_set, &test->write_set, sizeof(fd_set));
        tv.tv_sec = 15;
        tv.tv_usec = 0;

        result = select(test->max_fd + 1, &temp_read_set, &temp_write_set, NULL, &tv);
        if (result < 0 && errno != EINTR) {
            perror("select");
            exit(1);
        } else if (result > 0) {
            if (FD_ISSET(test->ctrl_sck, &temp_read_set)) {
                iperf_handle_message_client(test);
                FD_CLR(test->ctrl_sck, &temp_read_set);
            }

            if (test->reverse) {
                // Reverse mode. Client receives.
                iperf_recv(test);
            } else {
                // Regular mode. Client sends.
                iperf_send(test);
            }
        }
    }

    /* End the iperf test and clean up client specific memory */
    /* XXX: Commented out until fixed. Probably should be controlled by ctrl_sck
    if (iperf_client_end(test) < 0) {
        return -1;
    }
    */

    return 0;
}
