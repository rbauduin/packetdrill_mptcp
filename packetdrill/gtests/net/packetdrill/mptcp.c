/**
 * Authors: Arnaud Schils
 */

#include "mptcp.h"
#include "packet_to_string.h"

void init_mp_state()
{
	mp_state.packetdrill_key_set = false;
	mp_state.kernel_key_set = false;
	queue_init(&mp_state.vars_queue);
	mp_state.vars = NULL; //Init hashmap
	mp_state.last_packetdrill_addr_id = 0;
	mp_state.idsn = UNDEFINED;
	mp_state.remote_idsn = UNDEFINED;
	mp_state.remote_ssn = 0;
	mp_state.last_dsn_rcvd = 0;
}

void free_mp_state(){
	free_var_queue();
	free_vars();
	free_flows();
}

/**
 * Remember mptcp connection key generated by packetdrill. This key is needed
 * during the entire mptcp connection and is common among all mptcp subflows.
 */
void set_packetdrill_key(u64 sender_key)
{
	mp_state.packetdrill_key = sender_key;
	mp_state.packetdrill_key_set = true;
}

/**
 * Remember mptcp connection key generated by kernel. This key is needed
 * during the entire mptcp connection and is common among all mptcp subflows.
 */
void set_kernel_key(u64 receiver_key)
{
    mp_state.kernel_key = receiver_key;
    mp_state.kernel_key_set = true;
}

/* var_queue functions */

/**
 * Insert a COPY of name char* in mp_state.vars_queue.
 * Error is returned if queue is full.
 *
 */
int enqueue_var(char *name)
{
	unsigned name_length = strlen(name);
	char *new_el = malloc(sizeof(char)*name_length);
	memcpy(new_el, name, name_length);
	int full_err = queue_enqueue(&mp_state.vars_queue, new_el);
	return full_err;
}

//Caller should free
int dequeue_var(char **name){
	int empty_err = queue_dequeue(&mp_state.vars_queue, (void**)name);
	return empty_err;
}

//Free all variables names (char*) in vars_queue
void free_var_queue()
{
	queue_free(&mp_state.vars_queue);
}

/* hashmap functions */

void save_mp_var_name(char *name, struct mp_var *var)
{
	unsigned name_length = strlen(name);
	var->name = malloc(sizeof(char)*(name_length+1));
	memcpy(var->name, name, (name_length+1)); //+1 to copy '/0' too
}

/**
 *
 * Save a variable <name, value> in variables hashmap.
 * Where value is of u64 type key.
 *
 * Key memory location should stay valid, name is copied.
 *
 */
void add_mp_var_key(char *name, u64 *key)
{
	struct mp_var *var = malloc(sizeof(struct mp_var));
	save_mp_var_name(name, var);
	var->value = key;
	var->mptcp_subtype = MP_CAPABLE_SUBTYPE;
	var->mp_capable_info.script_defined = false;
	add_mp_var(var);
}

/**
 * Save a variable <name, value> in variables hashmap.
 * Value is copied in a newly allocated pointer and will be freed when
 * free_vars function will be executed.
 *
 */
void add_mp_var_script_defined(char *name, void *value, u32 length)
{
	struct mp_var *var = malloc(sizeof(struct mp_var));
	save_mp_var_name(name, var);
	var->value = malloc(length);
	memcpy(var->value, value, length);
	var->mptcp_subtype = MP_CAPABLE_SUBTYPE;
	var->mp_capable_info.script_defined = true;
	add_mp_var(var);
}

/**
 * Add var to the variable hashmap.
 */
void add_mp_var(struct mp_var *var)
{
	HASH_ADD_KEYPTR(hh, mp_state.vars, var->name, strlen(var->name), var);
}

/**
 * Search in the hashmap for the value of the variable of name "name" and
 * return both variable - value (mp_var struct).
 * NULL is returned if not found
 */
struct mp_var *find_mp_var(char *name)
{
    struct mp_var *found;
    HASH_FIND_STR(mp_state.vars, name, found);
    return found;
}

/**
 * Gives next mptcp key value needed to insert variable values while processing
 * the packets.
 */
u64 *find_next_key(){
	char *var_name;
	if(dequeue_var(&var_name) || !var_name){
		return NULL;
	}

	struct mp_var *var = find_mp_var(var_name);
	free(var_name);
/*
	if(!var || var->mptcp_subtype != MP_CAPABLE_SUBTYPE){
		return NULL;
	}*/
	return (u64*)var->value;
}

/**
 * Iterate through hashmap, free mp_var structs and mp_var->name.
 * Value is not freed for KEY type, since values come from stack.
 */
void free_vars()
{
	struct mp_var *next, *var;
	var = mp_state.vars;

	while(var){
		next = var->hh.next;
		free(var->name);
		if(var->mptcp_subtype == MP_CAPABLE_SUBTYPE){
			if(var->mp_capable_info.script_defined)
				free(var->value);
		}
		free(var);
		var = next;
	}
}

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
struct mp_subflow *new_subflow_inbound(struct packet *inbound_packet)
{

	struct mp_subflow *subflow = malloc(sizeof(struct mp_subflow));

	if(inbound_packet->ipv4){
		ip_from_ipv4(&inbound_packet->ipv4->src_ip, &subflow->src_ip);
		ip_from_ipv4(&inbound_packet->ipv4->dst_ip, &subflow->dst_ip);
	}

	else if(inbound_packet->ipv6){
		ip_from_ipv6(&inbound_packet->ipv6->src_ip, &subflow->src_ip);
		ip_from_ipv6(&inbound_packet->ipv6->dst_ip, &subflow->dst_ip );
	}

	else{
		return NULL;
	}

	subflow->src_port =	ntohs(inbound_packet->tcp->src_port);
	subflow->dst_port = ntohs(inbound_packet->tcp->dst_port);
	subflow->packetdrill_rand_nbr =	generate_32();
	subflow->packetdrill_addr_id = mp_state.last_packetdrill_addr_id;
	mp_state.last_packetdrill_addr_id++;
	subflow->ssn = 0;
//	subflow->state = UNDEFINED;  // TODO to define it and change the state after
	subflow->next = mp_state.subflows;
	mp_state.subflows = subflow;

	return subflow;
}

struct mp_subflow *new_subflow_outbound(struct packet *outbound_packet)
{

	struct mp_subflow *subflow = malloc(sizeof(struct mp_subflow));
	struct tcp_option *mp_join_syn =
			get_tcp_option(outbound_packet, TCPOPT_MPTCP);

	if(!mp_join_syn)
		return NULL;

	if(outbound_packet->ipv4){
		ip_from_ipv4(&outbound_packet->ipv4->dst_ip, &subflow->src_ip);
		ip_from_ipv4(&outbound_packet->ipv4->src_ip, &subflow->dst_ip);
	}

	else if(outbound_packet->ipv6){
		ip_from_ipv6(&outbound_packet->ipv6->dst_ip, &subflow->src_ip);
		ip_from_ipv6(&outbound_packet->ipv6->src_ip, &subflow->dst_ip);
	}

	else{
		return NULL;
	}

	subflow->src_port =	ntohs(outbound_packet->tcp->dst_port);
	subflow->dst_port = ntohs(outbound_packet->tcp->src_port);
	subflow->kernel_rand_nbr =
			mp_join_syn->data.mp_join.syn.no_ack.sender_random_number;
	subflow->kernel_addr_id =
			mp_join_syn->data.mp_join.syn.address_id;
	subflow->ssn = 0;
	subflow->next = mp_state.subflows;
	mp_state.subflows = subflow;

	return subflow;
}

/**
 * Return the first subflow S of mp_state.subflows for which match(packet, S)
 * returns true.
 */
struct mp_subflow *find_matching_subflow(struct packet *packet,
		bool (*match)(struct mp_subflow*, struct packet*))
{
	struct mp_subflow *subflow = mp_state.subflows;
	while(subflow){
		if((*match)(subflow, packet)){
			return subflow;
		}
		subflow = subflow->next;
	}
	return NULL;
}

static bool does_subflow_match_outbound_packet(struct mp_subflow *subflow,
		struct packet *outbound_packet){
	return subflow->dst_port == ntohs(outbound_packet->tcp->src_port) &&
			subflow->src_port == ntohs(outbound_packet->tcp->dst_port);
}

struct mp_subflow *find_subflow_matching_outbound_packet(
		struct packet *outbound_packet)
{
	return find_matching_subflow(outbound_packet, does_subflow_match_outbound_packet);
}

static bool does_subflow_match_inbound_packet(struct mp_subflow *subflow,
		struct packet *inbound_packet){
	return subflow->dst_port == ntohs(inbound_packet->tcp->dst_port) &&
			subflow->src_port == ntohs(inbound_packet->tcp->src_port);
}

struct mp_subflow *find_subflow_matching_inbound_packet(
		struct packet *inbound_packet)
{
	return find_matching_subflow(inbound_packet, does_subflow_match_inbound_packet);
}

struct mp_subflow *find_subflow_matching_socket(struct socket *socket){
	struct mp_subflow *subflow = mp_state.subflows;
	while(subflow){
		if(subflow->dst_port == socket->live.remote.port &&
				subflow->src_port == socket->live.local.port){
			return subflow;
		}
		subflow = subflow->next;
	}
	return NULL;
}

/**
 * Free all mptcp subflows struct being a member of mp_state.subflows list.
 */
void free_flows(){
	struct mp_subflow *subflow = mp_state.subflows;
	struct mp_subflow *temp;
	while(subflow){
		temp = subflow->next;
		free(subflow);
		subflow = temp;
	}
}

/**
 * Generate a mptcp packetdrill side key and save it for later reference in
 * the script.
 *
 */
int mptcp_gen_key()
{

	//Retrieve variable name parsed by bison.
	char *snd_var_name;
	if(queue_front(&mp_state.vars_queue, (void**)&snd_var_name))
		return STATUS_ERR;

	//Is that var has already a value assigned in the script by user, or should
	//we generate a mptcp key ourselves?
	struct mp_var *snd_var = find_mp_var(snd_var_name);

	if(snd_var && snd_var->mptcp_subtype == MP_CAPABLE_SUBTYPE &&
			snd_var->mp_capable_info.script_defined)
		set_packetdrill_key(*(u64*)snd_var->value);

	//First inbound mp_capable, generate new key
	//and save corresponding variable
	if(!mp_state.packetdrill_key_set){
		seed_generator();
		u64 key = rand_64();
		set_packetdrill_key(key);
		add_mp_var_key(snd_var_name, &mp_state.packetdrill_key);
	}

	return STATUS_OK;
}

/**
 * Insert key field value of mp_capable_syn mptcp option according to variable
 * specified in user script.
 *
 */
int mptcp_set_mp_cap_syn_key(struct tcp_option *tcp_opt)
{
	u64 *key = find_next_key();
	if(!key)
		return STATUS_ERR;
	tcp_opt->data.mp_capable.syn.key = *key;
	return STATUS_OK;
}

/**
 * Insert keys fields values of mp_capable mptcp option according to variables
 * specified in user script.
 */
int mptcp_set_mp_cap_keys(struct tcp_option *tcp_opt)
{
	u64 *key = find_next_key();
	if(!key)
		return STATUS_ERR;
	tcp_opt->data.mp_capable.no_syn.sender_key = *key;

	key = find_next_key();
	if(!key)
		return STATUS_ERR;
	tcp_opt->data.mp_capable.no_syn.receiver_key = *key;
	return STATUS_OK;
}

/**
 * Extract mptcp connection informations from mptcp packets sent by kernel.
 * (For example kernel mptcp key).
 */
static int extract_and_set_kernel_key(
		struct packet *live_packet)
{

	struct tcp_option* mpcap_opt =
			get_tcp_option(live_packet, TCPOPT_MPTCP);

	if(!mpcap_opt)
		return STATUS_ERR;

	//Check if kernel key hasn't been specified by user in script
	char *var_name;
	if(!queue_front(&mp_state.vars_queue, (void**)&var_name)){
		struct mp_var *var = find_mp_var(var_name);
		if(var && var->mptcp_subtype == MP_CAPABLE_SUBTYPE &&
				var->mp_capable_info.script_defined)
			set_kernel_key(*(u64*)var->value);
	}

	if(!mp_state.kernel_key_set){

		//Set found kernel key
		set_kernel_key(mpcap_opt->data.mp_capable.syn.key);
		//Set front queue variable name to refer to kernel key
		char *var_name;
		if(queue_front(&mp_state.vars_queue, (void**)&var_name)){
			return STATUS_ERR;
		}
		add_mp_var_key(var_name, &mp_state.kernel_key);
	}

	return STATUS_OK;
}

/**
 * Insert appropriate key in mp_capable mptcp option.
 */
int mptcp_subtype_mp_capable(struct packet *packet_to_modify,
		struct packet *live_packet,
		struct tcp_option *tcp_opt_to_modify,
		unsigned direction)
{
	int error;
	// Syn packet, packetdril -> kernel
	if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE_SYN &&
			direction == DIRECTION_INBOUND &&
			!packet_to_modify->tcp->ack){
		error = mptcp_gen_key();
		error = mptcp_set_mp_cap_syn_key(tcp_opt_to_modify) || error;
	}
	// Syn and Syn_ack kernel->packetdrill
	else if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE_SYN &&
			direction == DIRECTION_OUTBOUND){
		error = extract_and_set_kernel_key(live_packet);
		error = mptcp_set_mp_cap_syn_key(tcp_opt_to_modify);
		mp_state.remote_ssn++;
	}
	// Third (ack) packet in three-hand shake
	else if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE ){
		error = mptcp_set_mp_cap_keys(tcp_opt_to_modify);
		// Automatically put the idsn tokens
		mp_state.idsn = sha1_least_64bits(mp_state.packetdrill_key);
		mp_state.remote_idsn = sha1_least_64bits(mp_state.kernel_key);
		mp_state.last_dsn_rcvd = mp_state.remote_idsn+mp_state.remote_ssn;

		if(direction == DIRECTION_INBOUND)
			new_subflow_inbound(packet_to_modify);
		else if(direction == DIRECTION_OUTBOUND)
			new_subflow_outbound(packet_to_modify);
		else
			return STATUS_ERR;
	}
	// SYN_ACK, packetdrill->kernel
	else if(tcp_opt_to_modify->length == TCPOLEN_MP_CAPABLE_SYN &&
			direction == DIRECTION_INBOUND &&
			packet_to_modify->tcp->ack){
		error = mptcp_gen_key();
		error = mptcp_set_mp_cap_syn_key(tcp_opt_to_modify) || error;
	}

	else{
		return STATUS_ERR;
	}
	return error;
}

/**
 * Set appropriate receiver token value in tcp_option.
 *
 */
static void mp_join_syn_rcv_token(struct tcp_option *tcp_opt_to_modify,
		struct mp_join_info *mp_join_script_info,
		unsigned direction)
{
	if(mp_join_script_info->syn_or_syn_ack.is_script_defined){

		if(mp_join_script_info->syn_or_syn_ack.is_var){
			struct mp_var *var = find_mp_var(mp_join_script_info->syn_or_syn_ack.var);
			tcp_opt_to_modify->data.mp_join.syn.no_ack.receiver_token =
					htonl(sha1_least_32bits(*(u64*)var->value));
		}
		else{
			tcp_opt_to_modify->data.mp_join.syn.no_ack.receiver_token =
					htonl(mp_join_script_info->syn_or_syn_ack.hash);
		}
	}
	else if(direction == DIRECTION_INBOUND){
		tcp_opt_to_modify->data.mp_join.syn.no_ack.receiver_token =
				htonl(sha1_least_32bits(mp_state.kernel_key));
	}
	else if(direction == DIRECTION_OUTBOUND){
		tcp_opt_to_modify->data.mp_join.syn.no_ack.receiver_token =
				htonl(sha1_least_32bits(mp_state.packetdrill_key));
	}
}

static void mp_join_syn_address_id(struct tcp_option *tcp_opt_to_modify,
		struct mp_join_info *mp_join_script_info,
		struct mp_subflow *subflow,
		unsigned direction)
{
	if(mp_join_script_info->syn_or_syn_ack.address_id_script_defined){
		u8 script_addr_id = mp_join_script_info->syn_or_syn_ack.address_id;
		if(direction == DIRECTION_INBOUND )
			subflow->packetdrill_addr_id = script_addr_id;
		else
			subflow->kernel_addr_id = script_addr_id;
	}
	if(direction == DIRECTION_INBOUND){
		tcp_opt_to_modify->data.mp_join.syn.address_id =
				subflow->packetdrill_addr_id;
	}
	else if(direction == DIRECTION_OUTBOUND){
		tcp_opt_to_modify->data.mp_join.syn.address_id =
				subflow->kernel_addr_id;
	}
}

static void mp_join_syn_rand(struct tcp_option *tcp_opt_to_modify,
		struct mp_join_info *mp_join_script_info,
		struct mp_subflow *subflow,
		unsigned direction)
{
	//Set sender random number value
	if(mp_join_script_info->syn_or_syn_ack.rand_script_defined){
		u32 script_rand = mp_join_script_info->syn_or_syn_ack.rand;
		if(direction == DIRECTION_INBOUND)
			subflow->packetdrill_rand_nbr = script_rand;
		else
			subflow->kernel_rand_nbr = script_rand;
	}
	if(direction == DIRECTION_INBOUND){
		tcp_opt_to_modify->data.mp_join.syn.no_ack.sender_random_number =
				subflow->packetdrill_rand_nbr;
	}
	else if(direction == DIRECTION_OUTBOUND){
		tcp_opt_to_modify->data.mp_join.syn.no_ack.sender_random_number =
				htonl(subflow->kernel_rand_nbr);
	}
}

/**
 * Manage the case when packetdrill send a mp_join_syn to the kernel.
 *
 */
static int mp_join_syn(struct packet *packet_to_modify,
		struct packet *live_packet,
		struct tcp_option *tcp_opt_to_modify,
		struct mp_join_info *mp_join_script_info,
		unsigned direction)
{
	struct mp_subflow *subflow;
	if(direction == DIRECTION_INBOUND)
		subflow = new_subflow_inbound(packet_to_modify);
	else if(direction == DIRECTION_OUTBOUND)
		subflow = new_subflow_outbound(live_packet);
	if(!subflow)
		return STATUS_ERR;

	mp_join_syn_rcv_token(tcp_opt_to_modify, mp_join_script_info, direction);
	mp_join_syn_rand(tcp_opt_to_modify,
			mp_join_script_info,
			subflow,
			direction);
	mp_join_syn_address_id(tcp_opt_to_modify,
			mp_join_script_info,
			subflow,
			direction);

	return STATUS_OK;
}

void mp_join_syn_ack_sender_hmac(struct tcp_option *tcp_opt_to_modify,
		u64 key1, u64 key2, u32 msg1, u32 msg2)
{
	//Build key for HMAC-SHA1
	unsigned char hmac_key[16];
	unsigned long *key_a = (unsigned long*)hmac_key;
	unsigned long *key_b = (unsigned long*)&(hmac_key[8]);
	*key_a = key1;
	*key_b = key2;

	//Build message for HMAC-SHA1
	u32 msg[2];
	msg[0] = msg1;
	msg[1] = msg2;
	tcp_opt_to_modify->data.mp_join.syn.ack.sender_hmac =
			htobe64(hmac_sha1_truncat_64(hmac_key,
					16,
					(char*)msg,
					8));
}

static int mp_join_syn_ack(struct packet *packet_to_modify,
		struct packet *live_packet,
		struct tcp_option *tcp_opt_to_modify,
		struct mp_join_info *mp_join_script_info,
		unsigned direction)
{
	if(direction == DIRECTION_INBOUND){
		struct mp_subflow *subflow =
				find_subflow_matching_inbound_packet(packet_to_modify);

		if(!subflow)
			return STATUS_ERR;

		subflow->packetdrill_rand_nbr = generate_32();

		mp_join_syn_address_id(tcp_opt_to_modify,
				mp_join_script_info,
				subflow,
				direction);
		mp_state.last_packetdrill_addr_id++;

		if(mp_join_script_info->syn_or_syn_ack.rand_script_defined)
			subflow->packetdrill_rand_nbr =
					mp_join_script_info->syn_or_syn_ack.rand;

		tcp_opt_to_modify->data.mp_join.syn.ack.sender_random_number =
				htonl(subflow->packetdrill_rand_nbr);

		if(mp_join_script_info->syn_or_syn_ack.is_script_defined){
			if(mp_join_script_info->syn_or_syn_ack.is_var){
				struct mp_var *var =
						find_mp_var(mp_join_script_info->syn_or_syn_ack.var);
				struct mp_var *var2 =
						find_mp_var(mp_join_script_info->syn_or_syn_ack.var2);
				mp_join_syn_ack_sender_hmac(tcp_opt_to_modify,
									*(u64*)var->value,
									*(u64*)var2->value,
									subflow->packetdrill_rand_nbr,
									subflow->kernel_rand_nbr);

			}
		}
		else{
			mp_join_syn_ack_sender_hmac(tcp_opt_to_modify,
					mp_state.packetdrill_key,
					mp_state.kernel_key,
					subflow->packetdrill_rand_nbr,
					subflow->kernel_rand_nbr);
		}
	}

	else if(direction == DIRECTION_OUTBOUND){
		struct mp_subflow *subflow =
				find_subflow_matching_outbound_packet(live_packet);
		struct tcp_option *live_mp_join =
				get_tcp_option(live_packet, TCPOPT_MPTCP);

		if(!subflow || !live_mp_join)
			return STATUS_ERR;

		//Update mptcp packetdrill state
		subflow->kernel_addr_id =
				live_mp_join->data.mp_join.syn.address_id;
		subflow->kernel_rand_nbr =
				live_mp_join->data.mp_join.syn.ack.sender_random_number;

		//Build key for HMAC-SHA1
		unsigned char hmac_key[16];
		unsigned long *key_b = (unsigned long*)hmac_key;
		unsigned long *key_a = (unsigned long*)&(hmac_key[8]);
		*key_b = mp_state.kernel_key;
		*key_a = mp_state.packetdrill_key;

		//Build message for HMAC-SHA1
		unsigned msg[2];
		msg[0] = subflow->kernel_rand_nbr;
		msg[1] = subflow->packetdrill_rand_nbr;

		//Update script packet mp_join option fields
		tcp_opt_to_modify->data.mp_join.syn.address_id =
				live_mp_join->data.mp_join.syn.address_id;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_random_number =
				live_mp_join->data.mp_join.syn.ack.sender_random_number;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_hmac =
				hmac_sha1_truncat_64(hmac_key, 16, (char*)msg, 8);
	}
	return STATUS_OK;
}

/**
 * Update mptcp subflows state according to sent/sniffed mp_join packets.
 * Insert appropriate values retrieved from this up-to-date state in inbound
 * and outbound packets.
 */
int mptcp_subtype_mp_join(struct packet *packet_to_modify,
						struct packet *live_packet,
						struct tcp_option *tcp_opt_to_modify,
						unsigned direction)
{
	struct mp_join_info *mp_join_script_info;
	if(queue_dequeue(&mp_state.vars_queue, (void**)&mp_join_script_info))
		return STATUS_ERR;

	if(direction == DIRECTION_INBOUND &&
			!packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN){

		mp_join_syn(packet_to_modify,
				live_packet,
				tcp_opt_to_modify,
				mp_join_script_info,
				DIRECTION_INBOUND);
	}

	else if(direction == DIRECTION_OUTBOUND &&
			packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN_ACK){

		struct mp_subflow *subflow =
				find_subflow_matching_outbound_packet(live_packet);
		struct tcp_option *live_mp_join =
				get_tcp_option(live_packet, TCPOPT_MPTCP);

		if(!subflow || !live_mp_join)
			return STATUS_ERR;

		//Update mptcp packetdrill state
		subflow->kernel_addr_id =
				live_mp_join->data.mp_join.syn.address_id;
		subflow->kernel_rand_nbr =
				live_mp_join->data.mp_join.syn.ack.sender_random_number;

		//Build key for HMAC-SHA1
		unsigned char hmac_key[16];
		unsigned long *key_b = (unsigned long*)hmac_key;
		unsigned long *key_a = (unsigned long*)&(hmac_key[8]);
		*key_b = mp_state.kernel_key;
		*key_a = mp_state.packetdrill_key;

		//Build message for HMAC-SHA1
		unsigned msg[2];
		msg[0] = subflow->kernel_rand_nbr;
		msg[1] = subflow->packetdrill_rand_nbr;
		
		//Update script packet mp_join option fields
		tcp_opt_to_modify->data.mp_join.syn.address_id =
				live_mp_join->data.mp_join.syn.address_id;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_random_number =
				live_mp_join->data.mp_join.syn.ack.sender_random_number;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_hmac =
				hmac_sha1_truncat_64(hmac_key,
						16,
						(char*)msg,
						8);
	}

	else if(direction == DIRECTION_INBOUND &&
			packet_to_modify->tcp->ack &&
			!packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_ACK){
		struct mp_subflow *subflow = find_subflow_matching_inbound_packet(packet_to_modify);
		if(!subflow)
			return STATUS_ERR;

		//Build HMAC-SHA1 key
		unsigned char hmac_key[16];
		unsigned long *key_a = (unsigned long*)hmac_key;
		unsigned long *key_b = (unsigned long*)&(hmac_key[8]);
		*key_a = mp_state.packetdrill_key;
		*key_b = mp_state.kernel_key;

		//Build HMAC-SHA1 message
		unsigned msg[2];
		msg[0] = subflow->packetdrill_rand_nbr;
		msg[1] = subflow->kernel_rand_nbr;

		u32 sender_hmac[5];
		hmac_sha1(hmac_key,
				16,
				(char*)msg,
				8,
				(unsigned char*)sender_hmac);
		memcpy(tcp_opt_to_modify->data.mp_join.no_syn.sender_hmac,
			   sender_hmac,
				20);
	}

	else if(direction == DIRECTION_OUTBOUND &&
			!packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN){

		mp_join_syn(packet_to_modify,
				live_packet,
				tcp_opt_to_modify,
				mp_join_script_info,
				DIRECTION_OUTBOUND);
	}

	else if(direction == DIRECTION_INBOUND &&
			packet_to_modify->tcp->ack &&
			packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_SYN_ACK){

		mp_join_syn_ack(packet_to_modify,
				live_packet,
				tcp_opt_to_modify,
				mp_join_script_info,
				DIRECTION_INBOUND);
		/*
		struct mp_subflow *subflow =
				find_subflow_matching_inbound_packet(packet_to_modify);

		if(!subflow)
			return STATUS_ERR;

		subflow->packetdrill_rand_nbr = generate_32();

		//Build key for HMAC-SHA1
		unsigned char hmac_key[16];
		unsigned long *key_a = (unsigned long*)hmac_key;
		unsigned long *key_b = (unsigned long*)&(hmac_key[8]);
		*key_a = mp_state.packetdrill_key;
		*key_b = mp_state.kernel_key;

		//Build message for HMAC-SHA1
		unsigned msg[2];
		msg[0] = subflow->packetdrill_rand_nbr;
		msg[1] = subflow->kernel_rand_nbr;

		tcp_opt_to_modify->data.mp_join.syn.address_id =
				mp_state.last_packetdrill_addr_id;
		mp_state.last_packetdrill_addr_id++;
		tcp_opt_to_modify->data.mp_join.syn.ack.sender_random_number =
				htonl(subflow->packetdrill_rand_nbr);

		tcp_opt_to_modify->data.mp_join.syn.ack.sender_hmac =
				htobe64(hmac_sha1_truncat_64(hmac_key,
						16,
						(char*)msg,
						8));*/
	}

	else if(direction == DIRECTION_OUTBOUND &&
			packet_to_modify->tcp->ack &&
			!packet_to_modify->tcp->syn &&
			tcp_opt_to_modify->length == TCPOLEN_MP_JOIN_ACK){

		struct mp_subflow *subflow =
				find_subflow_matching_outbound_packet(packet_to_modify);

		if(!subflow)
			return STATUS_ERR;


		//Build HMAC-SHA1 key
		unsigned char hmac_key[16];
		unsigned long *key_b = (unsigned long*)hmac_key;
		unsigned long *key_a = (unsigned long*)&(hmac_key[8]);
		*key_b = mp_state.kernel_key;
		*key_a = mp_state.packetdrill_key;

		//Build HMAC-SHA1 message
		unsigned msg[2];
		msg[0] = subflow->kernel_rand_nbr;
		msg[1] = subflow->packetdrill_rand_nbr;

		u32 sender_hmac[5];
		hmac_sha1(hmac_key,
				16,
				(char*)msg,
				8,
				(unsigned char*)sender_hmac);

		memcpy(tcp_opt_to_modify->data.mp_join.no_syn.sender_hmac,
				sender_hmac,
				20);
	}

	else{
		return STATUS_ERR;
	}
	return STATUS_OK;
}





int dss_inbound_parser(u16 tcp_payload_length, struct tcp_option *tcp_opt_to_modify){
/*		struct mp_subflow *subflow =
				find_subflow_matching_inbound_packet(packet_to_modify);
		tcp_opt_to_modify->data.dss.dsn.w_cs.ssn =
				htonl(subflow->ssn);
		subflow->ssn += tcp_payload_length;
*/
	// DSN8
	if(tcp_opt_to_modify->data.dss.flag_M){
		if(tcp_opt_to_modify->data.dss.flag_m){
	/*	if(!tcp_opt_to_modify->data.dss.flag_m &&
				tcp_opt_to_modify->length == TCPOLEN_DSS_DSN8){
			printf("Handle inbound packet, dsn: %llu \n", tcp_opt_to_modify->data.dss.dsn.dsn8);

			//Set dsn being value specified in script + initial dsn
			tcp_opt_to_modify->data.dss.dsn.dsn8 =
					htobe64(mp_state.idsn + mp_state.nb_pkt_rcvd); //tcp_opt_to_modify->data.dss.dsn.dsn8);

			tcp_opt_to_modify->data.dss.dsn.w_cs.dll =
					htons(tcp_payload_length);

			struct {
				u64 dsn;
				u32 ssn;
				u16 dll;
				u16 zeros;
			} buffer_checksum;

			//Compute checksum
			buffer_checksum.dsn = tcp_opt_to_modify->data.dss.dsn.dsn8;
			buffer_checksum.ssn =
					tcp_opt_to_modify->data.dss.dsn.w_cs.ssn;
			buffer_checksum.dll =
					tcp_opt_to_modify->data.dss.dsn.w_cs.dll;
			buffer_checksum.zeros = 0;
			//tcp_opt_to_modify->data.dss.dsn.w_cs.checksum =
			//		checksum((u16*)&buffer_checksum, sizeof(buffer_checksum));
			tcp_opt_to_modify->data.dss.dsn.w_cs.checksum = 0;
			tcp_opt_to_modify->data.dss.dsn.w_cs.checksum =
					checksum((u16*)packet_to_modify->tcp, packet_to_modify->ip_bytes - packet_ip_header_len(packet_to_modify)) +
					checksum((u16*)&buffer_checksum, sizeof(buffer_checksum));
			// DACK flag present
	**		if(tcp_opt_to_modify->data.dss.flag_A){
				if(tcp_opt_to_modify->data.dss.flag_a){
					if(tcp_opt_to_modify->data.dss.dack.dack8==UNDEFINED){
						tcp_opt_to_modify->data.dss.dack.dack8 =
							mp_state.remote_idsn + mp_state.nb_pkt_rcvd; //htobe64(
						//	tcp_opt_to_modify->data.dss.dack.dack8);
					}else if(tcp_opt_to_modify->data.dss.dack.dack8==SCRIPT_DEFINED){
						tcp_opt_to_modify->data.dss.dack.dack8 =
						scripted_var = find_next_key();
						if(!scripted_var)
							return STATUS_ERR;

						mp_state.remote_idsn = sha1_least_64bits(*scripted_var);
					}
				}else{
					if(tcp_opt_to_modify->data.dss.dack.dack4==UNDEFINED){
						tcp_opt_to_modify->data.dss.dack.dack4 =
							mp_state.remote_idsn + + mp_state.nb_pkt_rcvd; //htobe64(
						//	tcp_opt_to_modify->data.dss.dack.dack4);
					//	printf("DAKC4 is undefined, new %u\n", tcp_opt_to_modify->data.dss.dack.dack4);
					}
				}
			}
		****
//			printf("checksum %u\n",tcp_opt_to_modify->data.dss.dsn.w_cs.checksum);
		}
		// DSN8 without checksum
		else if(tcp_opt_to_modify->data.dss.flag_M &&
				tcp_opt_to_modify->length == TCPOLEN_DSS_DSN8_WOCS){

			//Set dsn being value specified in script + initial dsn

			//works for payload length = 0 or 1
			tcp_opt_to_modify->data.dss.dsn.dsn8 =
				htobe64(mp_state.idsn+
						tcp_opt_to_modify->data.dss.dsn.dsn8+1);

			tcp_opt_to_modify->data.dss.dsn.wo_cs.dll =
					htons(tcp_payload_length);

			struct mp_subflow *subflow =
					find_subflow_matching_inbound_packet(packet_to_modify);
			tcp_opt_to_modify->data.dss.dsn.wo_cs.ssn =
					htonl(subflow->ssn);
			subflow->ssn += tcp_payload_length;
			// TODO compute checksum
			****
			struct {
				u64 dsn;
				u32 ssn;
				u16 dll;
				u16 zeros;
			} buffer_checksum;

			//Compute checksum
			buffer_checksum.dsn = tcp_opt_to_modify->data.dss.dsn.dsn8;
			buffer_checksum.ssn =
					tcp_opt_to_modify->data.dss.dsn.w_cs.ssn;
			buffer_checksum.dll =
					tcp_opt_to_modify->data.dss.dsn.w_cs.dll;
			buffer_checksum.zeros = 0;
			//tcp_opt_to_modify->data.dss.dsn.w_cs.checksum =
			//		checksum((u16*)&buffer_checksum, sizeof(buffer_checksum));
			tcp_opt_to_modify->data.dss.dsn.w_cs.checksum = 0;
			tcp_opt_to_modify->data.dss.dsn.w_cs.checksum =
					checksum((u16*)packet_to_modify->tcp, packet_to_modify->ip_bytes - packet_ip_header_len(packet_to_modify)) +
					checksum((u16*)&buffer_checksum, sizeof(buffer_checksum));
			printf("checksum %u\n",tcp_opt_to_modify->data.dss.dsn.w_cs.checksum);
			*/
		}else if(!tcp_opt_to_modify->data.dss.flag_m){
			printf("DSN4");
		}
	}
	// IF ACK only
	else if(tcp_opt_to_modify->data.dss.flag_A){
		if(!tcp_opt_to_modify->data.dss.flag_a){
			if(tcp_opt_to_modify->data.dss.dack.dack4==UNDEFINED){
				tcp_opt_to_modify->data.dss.dack.dack4 = htonl(mp_state.last_dsn_rcvd) ;
				//	htonl(mp_state.remote_idsn + mp_state.remote_ssn); //htobe64(
				// printf("1029:last_dsn_received: %u\n", htonl(mp_state.last_dsn_rcvd));
			// if we gave a variable name in the script
			}else if(tcp_opt_to_modify->data.dss.dack.dack4==SCRIPT_DEFINED){
				u64 *key = find_next_key();
				if(!key)
					return STATUS_ERR;
				mp_state.remote_idsn = sha1_least_32bits(*key);
				tcp_opt_to_modify->data.dss.dack.dack4 = htobe32(sha1_least_32bits(*key)+mp_state.remote_ssn);
				printf("[mptcp.c:1037]mp_state.remote_idsn: %llu\n", (u64)mp_state.remote_idsn);
			}
		}
	}
	return 0;
}

int dss_outbound_parser(u16 tcp_payload_length, struct tcp_option *tcp_opt_to_modify){
/*		
	// if a packet is coming from kernel with DSN and DACK
	if(tcp_opt_to_modify->data.dss.flag_M && tcp_opt_to_modify->data.dss.flag_A){
		// if dsn4 and dack4
		if(!tcp_opt_to_modify->data.dss.flag_m && !tcp_opt_to_modify->data.dss.flag_a){
			struct tcp_option* dss_opt = get_tcp_option(live_packet, TCPOPT_MPTCP);
			//Set dsn being value specified in script + initial dsn
			if(tcp_opt_to_modify->data.dss.dack_dsn.dsn->dsn4==UNDEFINED){
				tcp_opt_to_modify->data.dss.dack_dsn.dsn->dsn4 = htonl(dss_opt->data.dss.dack_dsn.dsn->dsn4);
				mp_state.last_dsn_rcvd  = ntohl(dss_opt->data.dss.dack_dsn.dsn->dsn4);
				// printf("1053:last_dsn_received: %u\n", ntohl(dss_opt->data.dss.dack_dsn.dsn.dsn4));
				//	htonl((u32)((u32)mp_state.remote_idsn + (u32)mp_state.remote_ssn)); // XXX how to convert u64 in u32
			}
			
	//		printf("[mptcp.c:1087]dsn4: %u, ", htonl(tcp_opt_to_modify->data.dss.dack_dsn.dsn.dsn4));
		//	struct mp_subflow *subflow = find_subflow_matching_outbound_packet(live_packet);
			// Do we have a checksum or not
			if(tcp_opt_to_modify->length == TCPOLEN_DSS_DACK4_DSN4){
				tcp_opt_to_modify->data.dss.dack_dsn.dsn->w_cs.ssn = htonl(dss_opt->data.dss.dack_dsn.dsn->w_cs.ssn); //htonl(mp_state.remote_ssn);
				tcp_opt_to_modify->data.dss.dack_dsn.dsn->w_cs.dll = htons(dss_opt->data.dss.dack_dsn.dsn->w_cs.dll); //htons(tcp_payload_length);
				tcp_opt_to_modify->data.dss.dack_dsn.dsn->w_cs.checksum = htons(dss_opt->data.dss.dack_dsn.dsn->w_cs.checksum); //htons(1111);
				
				// TODO compute checksum
	//			printf("ssn: %u, dll: %u, chk: %u",
	//					ntohl(tcp_opt_to_modify->data.dss.dack_dsn.dsn.w_cs.ssn),
	//					ntohs(tcp_opt_to_modify->data.dss.dack_dsn.dsn.w_cs.dll),
	//					ntohs(tcp_opt_to_modify->data.dss.dack_dsn.dsn.w_cs.checksum) );
			}else if(tcp_opt_to_modify->length == TCPOLEN_DSS_DACK4_DSN4_WOCS){
				tcp_opt_to_modify->data.dss.dack_dsn.dsn->wo_cs.ssn = htonl(dss_opt->data.dss.dack_dsn.dsn->wo_cs.ssn); //htonl(mp_state.remote_ssn);
				tcp_opt_to_modify->data.dss.dack_dsn.dsn->wo_cs.dll = htons(dss_opt->data.dss.dack_dsn.dsn->wo_cs.dll); //htons(tcp_payload_length);
				printf("ssn: %u, dll: %u",
						ntohl(tcp_opt_to_modify->data.dss.dack_dsn.dsn->wo_cs.ssn),
						ntohs(tcp_opt_to_modify->data.dss.dack_dsn.dsn->wo_cs.dll));
			}
			// if we have an undefined dack4
			if(tcp_opt_to_modify->data.dss.dack_dsn.dack->dack4 == UNDEFINED){
				tcp_opt_to_modify->data.dss.dack_dsn.dack->dack4 = dss_opt->data.dss.dack_dsn.dack->dack4; //htonl((u32)(mp_state.idsn + subflow->ssn));
			//	if(mp_state.last_dsn_rcvd < tcp_opt_to_modify->data.dss.dack.dack4)
			//		mp_state.last_dsn_rcvd = tcp_opt_to_modify->data.dss.dack.dack4;
			}else if(tcp_opt_to_modify->data.dss.dack_dsn.dack->dack4 == IGNORED){
				// Need it to do ? XXX
			}
	//		printf(", dack4: %u \n",ntohl(tcp_opt_to_modify->data.dss.dack_dsn.dack.dack4));
			if(!dss_opt->data.dss.flag_a && !dss_opt->data.dss.flag_m){
	//			printf("DSS_OPT=>dsn4: %u, ssn: %u, dll: %u, chk, %u, dack4: %u\n", 
	//					htonl(dss_opt->data.dss.dack_dsn.dsn.dsn4),
	//					htonl(dss_opt->data.dss.dack_dsn.dsn.w_cs.ssn),
	//					htons(dss_opt->data.dss.dack_dsn.dsn.w_cs.dll),
	//					htons(dss_opt->data.dss.dack_dsn.dsn.w_cs.checksum),
	//					htonl(dss_opt->data.dss.dack_dsn.dack.dack4)
				);
			}else{
				printf("A and M not deifned in dss_opt\n");
			}
		}
	
	// Only DSN is in the packet
	}else if(tcp_opt_to_modify->data.dss.flag_M ){ // DSS

		// if DSN is 8 octets
		if(tcp_opt_to_modify->data.dss.flag_m){
			//Set dsn being value specified in script + initial dsn
	***
			// if dsn8 is not given in the script, we'll put it automatically
			if(tcp_opt_to_modify->data.dss.dsn.dsn8==UNDEFINED){
				tcp_opt_to_modify->data.dss.dsn.dsn8 =
						htobe64(mp_state.idsn+mp_state.nb_pkt_rcvd); //htobe64
			}
			struct mp_subflow *subflow =
				find_subflow_matching_outbound_packet(live_packet);

			if(tcp_opt_to_modify->length == TCPOLEN_DSS_DSN8){
				tcp_opt_to_modify->data.dss.dsn.w_cs.dll =
						htons(tcp_payload_length); //htons(
				tcp_opt_to_modify->data.dss.dsn.w_cs.ssn =
						htonl(mp_state.nb_pkt_rcvd);
				tcp_opt_to_modify->data.dss.dsn.w_cs.checksum = 1111;

			}else if(tcp_opt_to_modify->length == TCPOLEN_DSS_DSN8_WOCS){
				tcp_opt_to_modify->data.dss.dsn.wo_cs.dll =
						htons(tcp_payload_length); //htons(

				tcp_opt_to_modify->data.dss.dsn.wo_cs.ssn =
						htonl(mp_state.nb_pkt_rcvd);
				tcp_opt_to_modify->data.dss.dsn.w_cs.checksum = 0;
			}
			printf("[1040]chk: %u, ", tcp_opt_to_modify->data.dss.dsn.w_cs.checksum);
			printf("dsn8: %u, ", tcp_opt_to_modify->data.dss.dsn.dsn4);
			printf("ssn: %u, dll: %u \n", subflow->ssn, tcp_payload_length);
	***
			printf("It is 8 octets\n");
		}
		// if DSN is 4 octets
		else {
			//Set dsn being value specified in script + initial dsn
			if(tcp_opt_to_modify->data.dss.dsn.dsn4==UNDEFINED){
				tcp_opt_to_modify->data.dss.dsn.dsn4 =
					htonl((u32)(mp_state.remote_idsn + mp_state.remote_ssn)); // XXX how to convert u64 in u32
			}
			printf("[mptcp.c:1087]dsn4: %u, ", htonl(tcp_opt_to_modify->data.dss.dsn.dsn4) );
			struct mp_subflow *subflow =
					find_subflow_matching_outbound_packet(live_packet);
			// Do we have a checksum or not
			if(tcp_opt_to_modify->length == TCPOLEN_DSS_DSN4){

				tcp_opt_to_modify->data.dss.dsn.w_cs.ssn = htonl(mp_state.remote_ssn);

				tcp_opt_to_modify->data.dss.dsn.w_cs.dll = htons(tcp_payload_length);
				tcp_opt_to_modify->data.dss.dsn.w_cs.checksum = htons(1111);
				// TODO compute checksum
				printf("ssn: %u, dll: %u, chk: %u",
						ntohl(tcp_opt_to_modify->data.dss.dsn.w_cs.ssn),
						ntohs(tcp_opt_to_modify->data.dss.dsn.w_cs.dll),
						ntohs(tcp_opt_to_modify->data.dss.dsn.w_cs.checksum) );

			}else if(tcp_opt_to_modify->length == TCPOLEN_DSS_DSN4_WOCS){
				tcp_opt_to_modify->data.dss.dsn.wo_cs.dll = htons(
					tcp_payload_length); //htons(

				tcp_opt_to_modify->data.dss.dsn.wo_cs.ssn =	htonl(mp_state.remote_ssn);

				printf("ssn: %u, dll: %u, no_chk ",
						tcp_opt_to_modify->data.dss.dsn.wo_cs.ssn,
						tcp_opt_to_modify->data.dss.dsn.wo_cs.dll );

			}
			// if we have a dack4
			if(!tcp_opt_to_modify->data.dss.flag_a){
				if(tcp_opt_to_modify->data.dss.dack.dack4 == UNDEFINED){
					tcp_opt_to_modify->data.dss.dack.dack4 = htonl((u32)(mp_state.idsn + subflow->ssn));
					if(mp_state.last_dsn_rcvd < tcp_opt_to_modify->data.dss.dack.dack4)
						mp_state.last_dsn_rcvd = tcp_opt_to_modify->data.dss.dack.dack4;
				}else if(tcp_opt_to_modify->data.dss.dack.dack4 == IGNORED){
					// Need it to do ? XXX
				}
				printf(", dack4: %u \n",ntohl(tcp_opt_to_modify->data.dss.dack.dack4));
			}else{
				if(tcp_opt_to_modify->data.dss.dack.dack8 == UNDEFINED){
					tcp_opt_to_modify->data.dss.dack.dack8 = be64toh((u64)(mp_state.idsn + subflow->ssn));
					if(mp_state.last_dsn_rcvd < tcp_opt_to_modify->data.dss.dack.dack8)
						mp_state.last_dsn_rcvd = tcp_opt_to_modify->data.dss.dack.dack8;
				}else if(tcp_opt_to_modify->data.dss.dack.dack8 == IGNORED){
					// Need it to do ? XXX
				}
			}

//			char *dump = NULL, *error = NULL;
//			packet_to_string(live_packet, DUMP_FULL, &dump, &error);
//			packet_to_string(live_packet, DUMP_FULL, &dump, &error);
//			printf("[1077]dsn4: %u, ", tcp_opt_to_modify->data.dss.dsn.dsn4);
//			printf("ssn: %u, dll: %u \n", subflow->ssn, tcp_payload_length);
		}
** 		printf("DACK: %d, ", tcp_opt_to_modify->data.dss.flag_A);
		printf("DACK8: %d, ", tcp_opt_to_modify->data.dss.flag_a);
		printf("DSN: %d, ", tcp_opt_to_modify->data.dss.flag_M);
		printf("DSN8: %d, ", tcp_opt_to_modify->data.dss.flag_m);
		printf("FIN: %d, ", tcp_opt_to_modify->data.dss.flag_F);
***

	// if it's DACK only from kernel
	}else if(tcp_opt_to_modify->data.dss.flag_A ){
		printf("Handle outbound packet, wrong one, -> nog te doen\n");
	}else{

	}
//	mp_state.nb_pkt_rcvd += tcp_payload_length;
*/	
	return 0;
}



int mptcp_subtype_dss(struct packet *packet_to_modify,
						struct packet *live_packet,
						struct tcp_option *tcp_opt_to_modify,
						unsigned direction){
	//Computer tcp payload length
	u16 packet_total_length = packet_to_modify->ip_bytes;
	u16 tcp_header_length = packet_to_modify->tcp->doff*4;
	u16 ip_header_length = packet_to_modify->ipv4->ihl*8;
	u16 tcp_header_wo_options = 20;
	u16 tcp_payload_length = packet_total_length-ip_header_length-
			(tcp_header_length-tcp_header_wo_options);

	// injecting a packet to kernel
	if(direction == DIRECTION_INBOUND){
		dss_inbound_parser(tcp_payload_length, tcp_opt_to_modify);
		
	}else if(direction == DIRECTION_OUTBOUND){
		dss_outbound_parser(tcp_payload_length, tcp_opt_to_modify);

	}

	else{
		return STATUS_ERR;
	}
	return STATUS_OK;
}

/**
 * Main function for managing mptcp packets. We have to insert appropriate
 * fields values for mptcp options according to previous state.
 *
 * Some of these values are generated randomly (packetdrill mptcp key,...)
 * others are sniffed from packets sent by the kernel (kernel mptcp key,...).
 * These values have to be inserted some mptcp script and live packets.
 */
int mptcp_insert_and_extract_opt_fields(struct packet *packet_to_modify,
		struct packet *live_packet, // could be the same as packet_to_modify
		unsigned direction)
{

	struct tcp_options_iterator tcp_opt_iter;
	struct tcp_option *tcp_opt_to_modify =
			tcp_options_begin(packet_to_modify, &tcp_opt_iter);
	int error;

	while(tcp_opt_to_modify != NULL){

		if(tcp_opt_to_modify->kind == TCPOPT_MPTCP){
			switch(tcp_opt_to_modify->data.mp_capable.subtype){

			case MP_CAPABLE_SUBTYPE:
				error = mptcp_subtype_mp_capable(packet_to_modify,
						live_packet,
						tcp_opt_to_modify,
						direction);
				break;

			case MP_JOIN_SUBTYPE:
				error = mptcp_subtype_mp_join(packet_to_modify,
						live_packet,
						tcp_opt_to_modify,
						direction);
				break;

			case DSS_SUBTYPE:
				error = mptcp_subtype_dss(packet_to_modify,
						live_packet,
						tcp_opt_to_modify,
						direction);
				break;

			default:
				error =  STATUS_ERR;
			}

			if(error)
				return STATUS_ERR;

		}

		tcp_opt_to_modify = tcp_options_next(&tcp_opt_iter, NULL);
	}

	return STATUS_OK;
}
