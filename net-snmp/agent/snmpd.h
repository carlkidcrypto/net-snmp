/*
 * snmpd.h
 */

extern int snmp_dump_packet;
extern int verbose;
extern int (*sd_handlers[])(int);

extern int snmp_read_packet (int);
extern void init_snmp2p (u_short);
extern void open_ports_snmp2p (void);
extern void send_trap_pdu (struct snmp_pdu *);
extern void send_easy_trap (int, int);
extern const u_char *getStatPtr (oid *, size_t *, u_char *, size_t *,
	u_short *, int, WriteMethod **write_method, struct snmp_pdu *, int *);
void init_agent (void);

/* config file parsing routines */
void snmpd_parse_config_authtrap (char *, char *);
void snmpd_parse_config_trapsink (char *, char *);
void snmpd_parse_config_trap2sink (char *, char *);
void snmpd_free_trapsinks (void);
void snmpd_parse_config_trapcommunity (char *, char *);
void snmpd_free_trapcommunity (void);
void agentBoots_conf (char *, char *);
