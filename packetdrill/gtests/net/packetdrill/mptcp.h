/**
 * This file contains all data structures needed to maintain mptcp state.
 *
 * Authors: Arnaud Schils
 *
 */

#ifndef __MPTCP_H__
#define __MPTCP_H__

#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "queue/queue.h"
#include "hashmap/uthash.h"
#include "utils.h"
#include "packet.h"
#include "socket.h"
#include "tcp_options.h"
#include "tcp_options_iterator.h"
#include "tcp_packet.h"
#include "run.h"
#include "packet_checksum.h"

#define MPTCP_VERSION 0

//MPTCP options subtypes
#define MP_CAPABLE_SUBTYPE 0
#define MP_JOIN_SUBTYPE 1
#define DSS_SUBTYPE 2
#define ADD_ADDR_SUBTYPE 3

/* MPTCP options subtypes length */
//MP_CAPABLE
#define TCPOLEN_MP_CAPABLE_SYN 12 /* Size of the first and second steps of the three way handshake. */
#define TCPOLEN_MP_CAPABLE 20 /* Size of the third step of the three way handshake. */
//MP_JOIN
#define TCPOLEN_MP_JOIN_SYN 12
#define TCPOLEN_MP_JOIN_SYN_ACK 16
#define TCPOLEN_MP_JOIN_ACK 24
//DSS
#define TCPOLEN_DSS_DACK4 8
#define TCPOLEN_DSS_DACK8 12
#define TCPOLEN_DSS_DSN4 16
#define TCPOLEN_DSS_DSN4_WOCS 14
#define TCPOLEN_DSS_DSN8 20
#define TCPOLEN_DSS_DSN8_WOCS 18
#define TCPOLEN_DSS_DACK4_DSN4 20
#define TCPOLEN_DSS_DACK4_DSN8 24
#define TCPOLEN_DSS_DACK8_DSN4 24
#define TCPOLEN_DSS_DACK8_DSN8 28
#define TCPOLEN_DSS_DACK4_DSN4_WOCS 18
#define TCPOLEN_DSS_DACK4_DSN8_WOCS 22
#define TCPOLEN_DSS_DACK8_DSN4_WOCS 22
#define TCPOLEN_DSS_DACK8_DSN8_WOCS 26
//ADD_ADDR
#define TCPOLEN_ADD_ADDR 8
#define TCPOLEN_ADD_ADDR_PORT 10

//MPTCP Flags
#define MP_CAPABLE_FLAGS 1
#define MP_CAPABLE_FLAGS_CS 129 //With checksum
#define MP_JOIN_SYN_FLAGS_BACKUP 1
#define MP_JOIN_SYN_FLAGS_NO_BACKUP 0
#define DSS_RESERVED 0

//Variable types
#define KEY 0
#define SCRIPT_DEFINED 1

struct mp_join_info {
	union {
		struct {
		bool address_id_script_defined;
		u8 address_id;
		bool is_script_defined;
		bool is_var;
		char var[255];
		char var2[255]; //TODO warning to input length
		u64 hash;
		bool rand_script_defined;
		u32 rand;
		} syn_or_syn_ack;
	};
};

//A script mptcp variable bring additional information from user script to
//mptcp.c.
struct mp_var {
	char *name;
	void *value;
	u8 mptcp_subtype;
	union {
		struct {
			bool script_defined;
		} mp_capable_info;
	};
	UT_hash_handle hh;
};

/**
 * Keep all info specific to a mptcp subflow
 */
struct mp_subflow {
	struct ip_address src_ip;
	struct ip_address dst_ip;
	u16 src_port;
	u16 dst_port;
	u8 packetdrill_addr_id;
	u8 kernel_addr_id;
	unsigned kernel_rand_nbr;
	unsigned packetdrill_rand_nbr;
	u32 subflow_sequence_number;
	struct mp_subflow *next;
};

/**
 * Global state for multipath TCP
 */
struct mp_state_s {
    u64 packetdrill_key; //packetdrill side key
    u64 kernel_key; //mptcp stack side key
    //Should be a single key for a mptcp session.
    bool packetdrill_key_set;
    bool kernel_key_set;

    /*
     * FIFO queue to track variables use. Once parser encounter a mptcp
     * variable, it will enqueue it in the var_queue. Since packets are
     * processed in the same order than their apparition in the script
     * we will dequeue the queue in run_packet.c functions to retrieve
     * needed variables, and then retrieve the corresponding values using
     * the hashmap.
     *
     */
    queue_t vars_queue;
    //hashmap, contains <key:variable_name, value: variable_value>
    struct mp_var *vars;
    struct mp_subflow *subflows;

    unsigned last_packetdrill_addr_id;

    u64 initial_dack;
    u64 initial_dsn;
};

typedef struct mp_state_s mp_state_t;

mp_state_t mp_state;

void init_mp_state();

void free_mp_state();

/**
 * Remember mptcp connection key generated by packetdrill. This key is needed
 * during the entire mptcp connection and is common among all mptcp subflows.
 */
void set_packetdrill_key(u64 packetdrill_key);

/**
 * Remember mptcp connection key generated by kernel. This key is needed
 * during the entire mptcp connection and is common among all mptcp subflows.
 */
void set_kernel_key(u64 kernel_key);


/* mp_var_queue functions */

/**
 * Insert a COPY of name char* in mp_state.vars_queue.
 * Error is returned if queue is full.
 *
 */
int enqueue_var(char *name);
//caller should free "name"
int dequeue_var(char **name);
//Free all variables names (char*) in vars_queue
void free_var_queue();

/* hashmap functions */

/**
 *
 * Save a variable <name, value> in variables hashmap.
 * Where value is of u64 type key.
 *
 * Key memory location should stay valid, name is copied.
 *
 */
void add_mp_var_key(char *name, u64 *key);

/**
 * Save a variable <name, value> in variables hashmap.
 * Value is copied in a newly allocated pointer and will be freed when
 * free_vars function will be executed.
 *
 */
void add_mp_var_script_defined(char *name, void *value, u32 length);

/**
 * Add var to the variable hashmap.
 */
void add_mp_var(struct mp_var *var);

/**
 * Search in the hashmap for the value of the variable of name "name" and
 * return both variable - value (mp_var struct).
 * NULL is returned if not found
 */
struct mp_var *find_mp_var(char *name);

/**
 * Gives next mptcp key value needed to insert variable values while processing
 * the packets.
 */
u64 *find_next_key();

/**
 * Iterate through hashmap, free mp_var structs and mp_var->name,
 * value is not freed since values come from stack.
 */
void free_vars();

/* subflows management */

/**
 * @pre inbound packet should be the first packet of a three-way handshake
 * mp_join initiated by packetdrill (thus an inbound mp_join syn packet).
 *
 * @post
 * - Create a new subflow structure containing all available information at this
 * time (src_ip, dst_ip, src_port, dst_port, packetdrill_rand_nbr,
 * packetdrill_addr_id). kernel_addr_id and kernel_rand_nbr should be set when
 * receiving syn+ack with mp_join mptcp option from kernel.
 *
 * - last_packetdrill_addr_id is incremented.
 */
struct mp_subflow *new_subflow_inbound(struct packet *packet);
struct mp_subflow *new_subflow_outbound(struct packet *outbound_packet);
/**
 * Return the first subflow S of mp_state.subflows for which match(packet, S)
 * returns true.
 */
struct mp_subflow *find_matching_subflow(struct packet *packet,
		bool (*match)(struct mp_subflow*, struct packet*));
struct mp_subflow *find_subflow_matching_outbound_packet(struct packet *outbound_packet);
struct mp_subflow *find_subflow_matching_socket(struct socket *socket);
struct mp_subflow *find_subflow_matching_inbound_packet(
		struct packet *inbound_packet);
/**
 * Free all mptcp subflows struct being a member of mp_state.subflows list.
 */
void free_flows();

/**
 * Generate a mptcp packetdrill side key and save it for later reference in
 * the script.
 */
int mptcp_gen_key();

/**
 * Insert key field value of mp_capable_syn mptcp option according to variable
 * specified in user script.
 *
 */
int mptcp_set_mp_cap_syn_key(struct tcp_option *tcp_opt);

/**
 * Insert keys fields values of mp_capable mptcp option according to variables
 * specified in user script.
 */
int mptcp_set_mp_cap_keys(struct tcp_option *tcp_opt);

/**
 * Insert appropriate key in mp_capable mptcp option.
 */
int mptcp_subtype_mp_capable(struct packet *packet,
		struct packet *live_packet,
		struct tcp_option *tcp_opt,
		unsigned direction);

/**
 * Update mptcp subflows state according to sent/sniffed mp_join packets.
 * Insert appropriate values retrieved from this up-to-date state in inbound
 * and outbound packets.
 */
int mptcp_subtype_mp_join(struct packet *packet,
						struct packet *live_packet,
						struct tcp_option *tcp_opt,
						unsigned direction);

int mptcp_subtype_dss(struct packet *packet_to_modify,
						struct packet *live_packet,
						struct tcp_option *tcp_opt_to_modify,
						unsigned direction);

/**
 * Main function for managing mptcp packets. We have to insert appropriate
 * fields values for mptcp options according to previous state and to extract
 * values from sniffed packets to update mptcp state.
 *
 * Some of these values are generated randomly (packetdrill mptcp key,...)
 * others are sniffed from packets sent by the kernel (kernel mptcp key,...).
 * These values have to be inserted some mptcp script and live packets.
 */
int mptcp_insert_and_extract_opt_fields(struct packet *packet_to_modify,
		struct packet *live_packet, // could be the same as packet_to_modify
		unsigned direction);

#endif /* __MPTCP_H__ */
