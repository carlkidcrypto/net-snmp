/*
 * agent_index.c
 *
 * Maintain a registry of index allocations
 *	(Primarily required for AgentX support,
 *	 but it could be more widely useable).
 */


#include <config.h>
#include <signal.h>
#if HAVE_STRING_H
#include <string.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
#if HAVE_WINSOCK_H
#include <winsock.h>
#endif
#if TIME_WITH_SYS_TIME
# ifdef WIN32
#  include <sys/timeb.h>
# else
#  include <sys/time.h>
# endif
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include "mibincl.h"
#include "snmp_client.h"
#include "default_store.h"
#include "ds_agent.h"
#include "callback.h"
#include "agent_callbacks.h"
#include "agent_index.h"
#include "snmp_alarm.h"

#include "snmpd.h"
#include "mibgroup/struct.h"
#include "mib_module_includes.h"

#ifdef USING_AGENTX_SUBAGENT_MODULE
#include "agentx/subagent.h"
#include "agentx/client.h"
#endif

	/*
	 * Initial support for index allocation
	 */

struct snmp_index {
    struct variable_list	*varbind;	/* or pointer to var_list ? */
    struct snmp_session		*session;	/* NULL implies unused  ? */
    struct snmp_index		*next_oid;
    struct snmp_index		*prev_oid;
    struct snmp_index		*next_idx;
} *snmp_index_head = NULL; 

extern struct snmp_session *main_session;

char *
register_string_index( oid *name, size_t name_len, char *cp )
{
    struct variable_list varbind, *res;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    varbind.type = ASN_OCTET_STR;
    snmp_set_var_objid( &varbind, name, name_len );
    if ( cp != ANY_STRING_INDEX ) {
        snmp_set_var_value( &varbind, (u_char *)cp, strlen(cp) );
	res = register_index( &varbind, ALLOCATE_THIS_INDEX, main_session );
    }
    else
	res = register_index( &varbind, ALLOCATE_ANY_INDEX, main_session );

    if ( res == NULL )
	return NULL;
    else
	return (char *)res->val.string;
}

int
register_int_index( oid *name, size_t name_len, int val )
{
    struct variable_list varbind, *res;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    varbind.type = ASN_INTEGER;
    snmp_set_var_objid( &varbind, name, name_len );
    varbind.val.string = varbind.buf;
    if ( val != ANY_INTEGER_INDEX ) {
        varbind.val_len = sizeof(long);
        *varbind.val.integer = val;
	res = register_index( &varbind, ALLOCATE_THIS_INDEX, main_session );
    }
    else
	res = register_index( &varbind, ALLOCATE_ANY_INDEX, main_session );

    if ( res == NULL )
	return -1;
    else
	return *res->val.integer;
}

struct variable_list *
register_oid_index( oid *name, size_t name_len,
		    oid *value, size_t value_len )
{
    struct variable_list varbind;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    varbind.type = ASN_OBJECT_ID;
    snmp_set_var_objid( &varbind, name, name_len );
    if ( value != ANY_OID_INDEX ) {
        snmp_set_var_value( &varbind, (u_char*)value, value_len*sizeof(oid) );
	return( register_index( &varbind, ALLOCATE_THIS_INDEX, main_session ));
    }
    else
	return( register_index( &varbind, ALLOCATE_ANY_INDEX, main_session ));
}

struct variable_list*
register_index(struct variable_list *varbind, int flags, struct snmp_session *ss )
{
    struct snmp_index *new_index, *idxptr, *idxptr2;
    struct snmp_index *prev_oid_ptr, *prev_idx_ptr;
    int res, res2, i;

#if defined(USING_AGENTX_SUBAGENT_MODULE) && !defined(TESTING)
    if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE) == SUB_AGENT )
	return( agentx_register_index( ss, varbind, flags ));
#endif
		/* Look for the requested OID entry */
    prev_oid_ptr = NULL;
    prev_idx_ptr = NULL;
    res  = 1;
    res2 = 1;
    for( idxptr = snmp_index_head ; idxptr != NULL;
			 prev_oid_ptr = idxptr, idxptr = idxptr->next_oid) {
	if ((res = snmp_oid_compare(varbind->name, varbind->name_length,
					idxptr->varbind->name,
					idxptr->varbind->name_length)) <= 0 )
		break;
    }

		/*  Found the OID - now look at the registered indices */
    if ( res == 0 && idxptr ) {
	if ( varbind->type != idxptr->varbind->type )
	    return NULL;		/* wrong type */

			/*
			 * If we've been asked for an arbitrary new value,
			 *	then find the end of the list.
			 * If we've been asked for any arbitrary value,
			 *	then look for an unused entry, and use that.
			 *      If there aren't any, continue as for new.
			 * Otherwise, locate the given value in the (sorted)
			 *	list of already allocated values
			 */
	if ( flags & ALLOCATE_ANY_INDEX ) {
            for(idxptr2 = idxptr ; idxptr2 != NULL;
		 prev_idx_ptr = idxptr2, idxptr2 = idxptr2->next_idx) {
		if ( flags == ALLOCATE_ANY_INDEX && idxptr2->session == NULL ) {
		    idxptr2->session = ss ;
		    return idxptr2->varbind;
		}
	    }
	}
	else {
            for(idxptr2 = idxptr ; idxptr2 != NULL;
		 prev_idx_ptr = idxptr2, idxptr2 = idxptr2->next_idx) {
	        switch ( varbind->type ) {
		    case ASN_INTEGER:
			res2 = (*varbind->val.integer - *idxptr2->varbind->val.integer);
			break;
		    case ASN_OCTET_STR:
			i = SNMP_MIN(varbind->val_len, idxptr2->varbind->val_len);
			res2 = memcmp(varbind->val.string, idxptr2->varbind->val.string, i);
			break;
		    case ASN_OBJECT_ID:
			res2 = snmp_oid_compare(varbind->val.objid, varbind->val_len/sizeof(oid),
					idxptr2->varbind->val.objid,
					idxptr2->varbind->val_len/sizeof(oid));
			break;
		    default:
	    		return NULL;		/* wrong type */
	        }
	        if ( res2 <= 0 )
		    break;
	    }
	    if ( res2 == 0 )
		return NULL;			/* duplicate value */
	}
    }

		/*
		 * OK - we've now located where the new entry needs to
		 *	be fitted into the index registry tree		
		 * To recap:
		 *	'prev_oid_ptr' points to the head of the OID index
		 *	    list prior to this one.  If this is null, then
		 *	    it means that this is the first OID in the list.
		 *	'idxptr' points either to the head of this OID list,
		 *	    or the next OID (if this is a new OID request)
		 *	    These can be distinguished by the value of 'res'.
		 *
		 *	'prev_idx_ptr' points to the index entry that sorts
		 *	    immediately prior to the requested value (if any).
		 *	    If an arbitrary value is required, then this will
		 *	    point to the last allocated index.
		 *	    If this pointer is null, then either this is a new
		 *	    OID request, or the requested value is the first
		 *	    in the list.
		 *	'idxptr2' points to the next sorted index (if any)
		 *	    but is not actually needed any more.
		 *
		 *  Clear?  Good!
		 *	I hope you've been paying attention.
		 *	    There'll be a test later :-)
		 */

		/*
		 *	We proceed by creating the new entry
		 *	   (by copying the entry provided)
		 */
	new_index = (struct snmp_index *)calloc( 1, sizeof( struct snmp_index ));
	if (new_index == NULL)
	    return NULL;

        if (0 == snmp_varlist_add_variable(&new_index->varbind,
		      varbind->name, 
		      varbind->name_length,
		      varbind->type,
		      varbind->val.string,
		      varbind->val_len)) {
/*	if (snmp_clone_var( varbind, new_index->varbind ) != 0 ) */
	    free( new_index );
	    return NULL;
	}
	new_index->session = ss;

	if ( varbind->type == ASN_OCTET_STR && flags == ALLOCATE_THIS_INDEX )
	    new_index->varbind->val.string[new_index->varbind->val_len] = 0;

		/*
		 * If we've been given a value, then we can use that, but
		 *    otherwise, we need to create a new value for this entry.
		 * Note that ANY_INDEX and NEW_INDEX are both covered by this
		 *   test (since NEW_INDEX & ANY_INDEX = ANY_INDEX, remember?)
		 */
	if ( flags & ALLOCATE_ANY_INDEX ) {
	    if ( prev_idx_ptr ) {
		if ( snmp_clone_var( prev_idx_ptr->varbind, new_index->varbind ) != 0 ) {
		    free( new_index );
		    return NULL;
		}
	    }
	    else
		new_index->varbind->val.string = new_index->varbind->buf;

	    switch ( varbind->type ) {
		case ASN_INTEGER:
		    if ( prev_idx_ptr ) {
			(*new_index->varbind->val.integer)++; 
		    }
		    else
			*(new_index->varbind->val.integer) = 1;
		    new_index->varbind->val_len = sizeof(long);
		    break;
		case ASN_OCTET_STR:
		    if ( prev_idx_ptr ) {
			i =  new_index->varbind->val_len-1;
			while ( new_index->varbind->buf[ i ] == 'z' ) {
			    new_index->varbind->buf[ i ] = 'a';
			    i--;
			    if ( i < 0 ) {
				i =  new_index->varbind->val_len;
			        new_index->varbind->buf[ i ] = 'a';
			        new_index->varbind->buf[ i+1 ] = 0;
			    }
			}
			new_index->varbind->buf[ i ]++;
		    }
		    else
		        strcpy((char *)new_index->varbind->buf, "aaaa");
		    new_index->varbind->val_len = strlen((char *)new_index->varbind->buf);
		    break;
		case ASN_OBJECT_ID:
		    if ( prev_idx_ptr ) {
			i =  prev_idx_ptr->varbind->val_len/sizeof(oid) -1;
			while ( new_index->varbind->val.objid[ i ] == 255 ) {
			    new_index->varbind->val.objid[ i ] = 1;
			    i--;
			    if ( i == 0 && new_index->varbind->val.objid[0] == 2 ) {
			        new_index->varbind->val.objid[ 0 ] = 1;
				i =  new_index->varbind->val_len/sizeof(oid);
			        new_index->varbind->val.objid[ i ] = 0;
				new_index->varbind->val_len += sizeof(oid);
			    }
			}
			new_index->varbind->val.objid[ i ]++;
		    }
		    else {
			/* If the requested OID name is small enough,
			 *   append another OID (1) and use this as the
			 *   default starting value for new indexes.
			 */
		        if ( (varbind->name_length+1) * sizeof(oid) <= 40 ) {
			    for ( i = 0 ; i < (int)varbind->name_length ; i++ )
			        new_index->varbind->val.objid[i] = varbind->name[i];
			    new_index->varbind->val.objid[varbind->name_length] = 1;
			    new_index->varbind->val_len =
					(varbind->name_length+1) * sizeof(oid);
		        }
		        else {
			    /* Otherwise use '.1.1.1.1...' */
			    i = 40/sizeof(oid);
			    if ( i > 4 )
				i = 4;
			    new_index->varbind->val_len = i * (sizeof(oid));
			    for (i-- ; i>=0 ; i-- )
			        new_index->varbind->val.objid[i] = 1;
		        }
		    }
		    break;
		default:
		    snmp_free_var(new_index->varbind);
		    free( new_index );
		    return NULL;	/* Index type not supported */
	    }
	}

		/*
		 * Right - we've set up the new entry.
		 * All that remains is to link it into the tree.
		 * There are a number of possible cases here,
		 *   so watch carefully.
		 */
	if ( prev_idx_ptr ) {
	    new_index->next_idx = prev_idx_ptr->next_idx;
	    new_index->next_oid = prev_idx_ptr->next_oid;
	    prev_idx_ptr->next_idx = new_index;
	}
	else {
	    if ( res == 0 && idxptr ) {
	        new_index->next_idx = idxptr;
	        new_index->next_oid = idxptr->next_oid;
	    }
	    else {
	        new_index->next_idx = NULL;
	        new_index->next_oid = idxptr;
	    }

	    if ( prev_oid_ptr ) {
		while ( prev_oid_ptr ) {
		    prev_oid_ptr->next_oid = new_index;
		    prev_oid_ptr = prev_oid_ptr->next_idx;
		}
	    }
	    else
	        snmp_index_head = new_index;
	}
    return new_index->varbind;
}

	/*
	 * Release an allocated index,
	 *   to allow it to be used elsewhere
	 */
int
release_index(struct variable_list *varbind)
{
    return( unregister_index( varbind, TRUE, NULL ));
}

	/*
	 * Completely remove an allocated index,
	 *   due to errors in the registration process.
	 */
int
remove_index(struct variable_list *varbind, struct snmp_session *ss)
{
    return( unregister_index( varbind, FALSE, ss ));
}

void
unregister_index_by_session(struct snmp_session *ss)
{
    struct snmp_index *idxptr, *idxptr2;
    for(idxptr = snmp_index_head ; idxptr != NULL; idxptr = idxptr->next_oid)
	for(idxptr2 = idxptr ; idxptr2 != NULL; idxptr2 = idxptr2->next_idx)
	    if ( idxptr2->session == ss )
		idxptr2->session = NULL;
}


int
unregister_index(struct variable_list *varbind, int remember, struct snmp_session *ss)
{
    struct snmp_index *idxptr, *idxptr2;
    struct snmp_index *prev_oid_ptr, *prev_idx_ptr;
    int res, res2, i;

#if defined(USING_AGENTX_SUBAGENT_MODULE) && !defined(TESTING)
    if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE) == SUB_AGENT )
	return( agentx_unregister_index( ss, varbind ));
#endif
		/* Look for the requested OID entry */
    prev_oid_ptr = NULL;
    prev_idx_ptr = NULL;
    res  = 1;
    res2 = 1;
    for( idxptr = snmp_index_head ; idxptr != NULL;
			 prev_oid_ptr = idxptr, idxptr = idxptr->next_oid) {
	if ((res = snmp_oid_compare(varbind->name, varbind->name_length,
					idxptr->varbind->name,
					idxptr->varbind->name_length)) <= 0 )
		break;
    }

    if ( res != 0 )
	return INDEX_ERR_NOT_ALLOCATED;
    if ( varbind->type != idxptr->varbind->type )
	return INDEX_ERR_WRONG_TYPE;

    for(idxptr2 = idxptr ; idxptr2 != NULL;
		prev_idx_ptr = idxptr2, idxptr2 = idxptr2->next_idx) {
	i = SNMP_MIN(varbind->val_len, idxptr2->varbind->val_len);
	res2 = memcmp(varbind->val.string, idxptr2->varbind->val.string, i);
	if ( res2 <= 0 )
	    break;
    }
    if ( res2 != 0 )
	return INDEX_ERR_NOT_ALLOCATED;
    if ( ss != idxptr2->session )
	return INDEX_ERR_WRONG_SESSION;

		/*
		 *  If this is a "normal" index unregistration,
		 *	mark the index entry as unused, but leave
		 *	it in situ.  This allows differentiation
		 *	between ANY_INDEX and NEW_INDEX
		 */
    if ( remember ) {
	idxptr2->session = NULL;	/* Unused index */
	return SNMP_ERR_NOERROR;
    }
		/*
		 *  If this is a failed attempt to register a
		 *	number of indexes, the successful ones
		 *	must be removed completely.
		 */
    if ( prev_idx_ptr ) {
	prev_idx_ptr->next_idx = idxptr2->next_idx;
    }
    else if ( prev_oid_ptr ) {
	if ( idxptr2->next_idx )	/* Use p_idx_ptr as a temp variable */
	    prev_idx_ptr = idxptr2->next_idx;
	else
	    prev_idx_ptr = idxptr2->next_oid;
	while ( prev_oid_ptr ) {
	    prev_oid_ptr->next_oid = prev_idx_ptr;
	    prev_oid_ptr = prev_oid_ptr->next_idx;
	}
    }
    else {
	if ( idxptr2->next_idx )
	    snmp_index_head = idxptr2->next_idx;
	else
	    snmp_index_head = idxptr2->next_oid;
    }
    snmp_free_var( idxptr2->varbind );
    free( idxptr2 );
    return SNMP_ERR_NOERROR;
}


void dump_idx_registry( void )
{
    struct snmp_index *idxptr, *idxptr2;
    char start_oid[SPRINT_MAX_LEN];
    char end_oid[SPRINT_MAX_LEN];

    if ( snmp_index_head )
	printf("\nIndex Allocations:\n");
    for( idxptr = snmp_index_head ; idxptr != NULL; idxptr = idxptr->next_oid) {
	sprint_objid(start_oid, idxptr->varbind->name, idxptr->varbind->name_length);
	printf("%s indexes:\n", start_oid);
        for( idxptr2 = idxptr ; idxptr2 != NULL; idxptr2 = idxptr2->next_idx) {
	    switch( idxptr2->varbind->type ) {
		case ASN_INTEGER:
		    printf("    %c %ld %c\n",
			( idxptr2->session ? ' ' : '(' ),
			  *idxptr2->varbind->val.integer,
			( idxptr2->session ? ' ' : ')' ));
		    break;
		case ASN_OCTET_STR:
		    printf("    %c %s %c\n",
			( idxptr2->session ? ' ' : '(' ),
			  idxptr2->varbind->val.string,
			( idxptr2->session ? ' ' : ')' ));
		    break;
		case ASN_OBJECT_ID:
		    sprint_objid(end_oid, idxptr2->varbind->val.objid,
				idxptr2->varbind->val_len/sizeof(oid));
		    printf("    %c %s %c\n",
			( idxptr2->session ? ' ' : '(' ),
			  end_oid,
			( idxptr2->session ? ' ' : ')' ));
		    break;
		default:
		    printf("unsupported type (%d)\n",
				idxptr2->varbind->type);
	    }
	}
    }
}

#ifdef TESTING
struct variable_list varbind;
struct snmp_session main_sess, *main_session=&main_sess;

void
test_string_register( int n, char *cp )
{
    varbind->name[4] = n;
    if (register_string_index(varbind->name, varbind.name_length, cp) == NULL)
	printf("allocating %s failed\n", cp);
}

void
test_int_register( int n, int val )
{
    varbind->name[4] = n;
    if (register_int_index( varbind->name, varbind.name_length, val ) == -1 )
	printf("allocating %d/%d failed\n", n, val);
}

void
test_oid_register( int n, int subid )
{
    struct variable_list *res;

    varbind->name[4] = n;
    if ( subid != -1 ) {
        varbind->val.objid[5] = subid;
	res = register_oid_index(varbind->name, varbind.name_length,
		    varbind->val.objid,
		    varbind->val_len/sizeof(oid) );
    }
    else
	res = register_oid_index(varbind->name, varbind.name_length, NULL, 0);

    if (res == NULL )
	printf("allocating %d/%d failed\n", n, subid);
}

void
main( int argc, char argv[] )
{
    oid name[] = { 1, 2, 3, 4, 0 };
    int i;
    
    memset( &varbind, 0, sizeof(struct variable_list));
    snmp_set_var_objid( &varbind, name, 5 );
    varbind->type = ASN_OCTET_STR;
		/*
		 * Test index structure linking:
		 *	a) sorted by OID
		 */
    test_string_register( 20, "empty OID" );
    test_string_register( 10, "first OID" );
    test_string_register( 40, "last OID" );
    test_string_register( 30, "middle OID" );

		/*
		 *	b) sorted by index value
		 */
    test_string_register( 25, "eee: empty IDX" );
    test_string_register( 25, "aaa: first IDX" );
    test_string_register( 25, "zzz: last IDX" );
    test_string_register( 25, "mmm: middle IDX" );
    printf("This next one should fail....\n");
    test_string_register( 25, "eee: empty IDX" );	/* duplicate */
    printf("done\n");

		/*
		 *	c) test initial index linking
		 */
    test_string_register( 5, "eee: empty initial IDX" );
    test_string_register( 5, "aaa: replace initial IDX" );

		/*
		 *	Did it all work?
		 */
    dump_idx_registry();
    unregister_index_by_session( main_session );
		/*
		 *  Now test index allocation
		 *	a) integer values
		 */
    test_int_register( 110, -1 );	/* empty */
    test_int_register( 110, -1 );	/* append */
    test_int_register( 110, 10 );	/* append exact */
    printf("This next one should fail....\n");
    test_int_register( 110, 10 );	/* exact duplicate */
    printf("done\n");
    test_int_register( 110, -1 );	/* append */
    test_int_register( 110,  5 );	/* insert exact */

		/*
		 *	b) string values
		 */
    test_string_register( 120, NULL );		/* empty */
    test_string_register( 120, NULL );		/* append */
    test_string_register( 120, "aaaz" );
    test_string_register( 120, NULL );		/* minor rollover */
    test_string_register( 120, "zzzz" );
    test_string_register( 120, NULL );		/* major rollover */

		/*
		 *	c) OID values
		 */
    
    test_oid_register( 130, -1 );	/* empty */
    test_oid_register( 130, -1 );	/* append */

    varbind->val_len = varbind.name_length*sizeof(oid);
    memcpy( varbind->buf, varbind.name, varbind.val_len);
    varbind->val.objid = (oid*) varbind.buf;
    varbind->val_len += sizeof(oid);

    test_oid_register( 130, 255 );	/* append exact */
    test_oid_register( 130, -1 );	/* minor rollover */
    test_oid_register( 130, 100 );	/* insert exact */
    printf("This next one should fail....\n");
    test_oid_register( 130, 100 );	/* exact duplicate */
    printf("done\n");

    varbind->val.objid = (oid*)varbind.buf;
    for ( i=0; i<6; i++ )
	varbind->val.objid[i]=255;
    varbind->val.objid[0]=1;
    test_oid_register( 130, 255 );	/* set up rollover  */
    test_oid_register( 130, -1 );	/* medium rollover */

    for ( i=0; i<6; i++ )
	varbind->val.objid[i]=255;
    varbind->val.objid[0]=2;
    test_oid_register( 130, 255 );	/* set up rollover  */
    test_oid_register( 130, -1 );	/* major rollover */

		/*
		 *	Did it all work?
		 */
    dump_idx_registry();

		/*
		 *	Test the various "invalid" requests
		 *	(unsupported types, mis-matched types, etc)
		 */
    printf("The rest of these should fail....\n");
    test_oid_register( 110, -1 );
    test_oid_register( 110, 100 );
    test_oid_register( 120, -1 );
    test_oid_register( 120, 100 );
    test_string_register( 110, NULL );
    test_string_register( 110, "aaaa" );
    test_string_register( 130, NULL );
    test_string_register( 130, "aaaa" );
    test_int_register( 120, -1 );
    test_int_register( 120,  1 );
    test_int_register( 130, -1 );
    test_int_register( 130,  1 );
    printf("done - this dump should be the same as before\n");
    dump_idx_registry();
}
#endif


int external_readfd[NUM_EXTERNAL_FDS], external_readfdlen = 0;
int external_writefd[NUM_EXTERNAL_FDS], external_writefdlen = 0;
int external_exceptfd[NUM_EXTERNAL_FDS], external_exceptfdlen = 0;
void (* external_readfdfunc[NUM_EXTERNAL_FDS])(int);
void (* external_writefdfunc[NUM_EXTERNAL_FDS])(int);
void (* external_exceptfdfunc[NUM_EXTERNAL_FDS])(int);

int register_readfd(int fd, void (*func)(int)) {
    if (external_readfdlen < NUM_EXTERNAL_FDS) {
	external_readfd[external_readfdlen] = fd;
	external_readfdfunc[external_readfdlen] = func;
	external_readfdlen++;
	DEBUGMSGTL(("register_readfd", "registered fd %d\n", fd));
	return FD_REGISTERED_OK;
    } else {
	snmp_log(LOG_CRIT, "register_readfd: too many file descriptors\n");
	return FD_REGISTRATION_FAILED;
    }
}

int register_writefd(int fd, void (*func)(int)) {
    if (external_writefdlen < NUM_EXTERNAL_FDS) {
	external_writefd[external_writefdlen] = fd;
	external_writefdfunc[external_writefdlen] = func;
	external_writefdlen++;
	DEBUGMSGTL(("register_writefd", "registered fd %d\n", fd));
	return FD_REGISTERED_OK;
    } else {
	snmp_log(LOG_CRIT, "register_writefd: too many file descriptors\n");
	return FD_REGISTRATION_FAILED;
    }
}

int register_exceptfd(int fd, void (*func)(int)) {
    if (external_exceptfdlen < NUM_EXTERNAL_FDS) {
	external_exceptfd[external_exceptfdlen] = fd;
	external_exceptfdfunc[external_exceptfdlen] = func;
	external_exceptfdlen++;
	DEBUGMSGTL(("register_exceptfd", "registered fd %d\n", fd));
	return FD_REGISTERED_OK;
    } else {
	snmp_log(LOG_CRIT, "register_exceptfd: too many file descriptors\n");
	return FD_REGISTRATION_FAILED;
    }
}

int unregister_readfd(int fd) {
    int i, j;

    for (i = 0; i < external_readfdlen; i++) {
	if (external_readfd[i] == fd) {
	    external_readfdlen--;
	    for (j = i; j < external_readfdlen; j++) {
		external_readfd[j] = external_readfd[j+1];
	    }
	    DEBUGMSGTL(("unregister_readfd", "unregistered fd %d\n", fd));
	    return FD_UNREGISTERED_OK;
	}
    }
    return FD_NO_SUCH_REGISTRATION;
}

int unregister_writefd(int fd) {
    int i, j;

    for (i = 0; i < external_writefdlen; i++) {
	if (external_writefd[i] == fd) {
	    external_writefdlen--;
	    for (j = i; j < external_writefdlen; j++) {
		external_writefd[j] = external_writefd[j+1];
	    }
	    DEBUGMSGTL(("unregister_writefd", "unregistered fd %d\n", fd));
	    return FD_UNREGISTERED_OK;
	}
    }
    return FD_NO_SUCH_REGISTRATION;
}

int unregister_exceptfd(int fd) {
    int i, j;

    for (i = 0; i < external_exceptfdlen; i++) {
	if (external_exceptfd[i] == fd) {
	    external_exceptfdlen--;
	    for (j = i; j < external_exceptfdlen; j++) {
		external_exceptfd[j] = external_exceptfd[j+1];
	    }
	    DEBUGMSGTL(("unregister_exceptfd", "unregistered fd %d\n", fd));
	    return FD_UNREGISTERED_OK;
	}
    }
    return FD_NO_SUCH_REGISTRATION;
}

int external_signal_scheduled[NUM_EXTERNAL_SIGS];
void (* external_signal_handler[NUM_EXTERNAL_SIGS])(int);

#ifndef WIN32

/*
 * TODO: add agent_SIGXXX_handler functions and `case SIGXXX: ...' lines
 *       below for every single that might be handled by register_signal().
 */

void agent_SIGCHLD_handler(void)
{
  external_signal_scheduled[SIGCHLD]++;
#ifndef HAVE_SIGACTION
  /* signal() sucks. It *might* have SysV semantics, which means that
   * a signal handler is reset once it gets called. Ensure that it
   * remains active.
   */
  signal(SIGCHLD, (void *)agent_SIGCHLD_handler);
#endif
}

int register_signal(int sig, void (*func)(int))
{

    switch (sig) {
#if defined(SIGCHLD)
    case SIGCHLD:
#ifdef HAVE_SIGACTION
	{
		static struct sigaction act;
		act.sa_handler = agent_SIGCHLD_handler;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		sigaction(SIGCHLD, &act, NULL);
	}
#else
	signal(SIGCHLD, (void *)agent_SIGCHLD_handler);
#endif
	break;
#endif
    default:
	snmp_log(LOG_CRIT,
		 "register_signal: signal %d cannot be handled\n", sig);
	return SIG_REGISTRATION_FAILED;
    }

    external_signal_handler[sig] = func;
    external_signal_scheduled[sig] = 0;
    
    DEBUGMSGTL(("register_signal", "registered signal %d\n", sig));
    return SIG_REGISTERED_OK;
}

int unregister_signal(int sig) {
    signal(sig, SIG_DFL);
    DEBUGMSGTL(("unregister_signal", "unregistered signal %d\n", sig));
    return SIG_UNREGISTERED_OK;
}

#endif /* !WIN32 */
