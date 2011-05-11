#include <config.h>
#include <stdlib.h>
#include <assert.h>

#include "dnslib/dnslib-common.h"
#include "dnslib/zone.h"
#include "dnslib/node.h"
#include "dnslib/dname.h"
#include "dnslib/consts.h"
#include "dnslib/descriptor.h"
#include "dnslib/nsec3.h"
#include "dnslib/error.h"
#include "dnslib/debug.h"
#include "dnslib/utils.h"
#include "common/tree.h"
#include "common/base32hex.h"
#include "dnslib/hash/cuckoo-hash-table.h"

/*----------------------------------------------------------------------------*/
/* Non-API functions                                                          */
/*----------------------------------------------------------------------------*/

// AVL tree functions
TREE_DEFINE(dnslib_node, avl);

/*----------------------------------------------------------------------------*/
/*!
 * \brief Checks if the given node can be inserted into the given zone.
 *
 * Checks if both the arguments are non-NULL and if the owner of the node
 * belongs to the zone (i.e. is a subdomain of the zone apex).
 *
 * \param zone Zone to which the node is going to be inserted.
 * \param node Node to check.
 *
 * \retval DNSLIB_EOK if both arguments are non-NULL and the node belongs to the
 *         zone.
 * \retval DNSLIB_EBADARG if either of the arguments is NULL.
 * \retval DNSLIB_EBADZONE if the node does not belong to the zone.
 */
static int dnslib_zone_check_node(const dnslib_zone_t *zone,
                                  const dnslib_node_t *node)
{
	if (zone == NULL || node == NULL) {
		return DNSLIB_EBADARG;
	}

	// assert or just check??
	assert(zone->apex != NULL);

	if (!dnslib_dname_is_subdomain(node->owner, zone->apex->owner)) {
DEBUG_DNSLIB_ZONE(
		char *node_owner = dnslib_dname_to_str(node->owner);
		char *apex_owner = dnslib_dname_to_str(zone->apex->owner);
		debug_dnslib_zone("zone: Trying to insert foreign node to a "
				  "zone. Node owner: %s, zone apex: %s\n",
				  node_owner, apex_owner);
		free(node_owner);
		free(apex_owner);
);
		return DNSLIB_EBADZONE;
	}
	return DNSLIB_EOK;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroys all RRSets in a node.
 *
 * This function is designed to be used in the tree-iterating functions.
 *
 * \param node Node to destroy RRSets from.
 * \param data Unused parameter.
 */
static void dnslib_zone_destroy_node_rrsets_from_tree(dnslib_node_t *node,
                                                      void *data)
{
	int free_rdata_dnames = (int)((intptr_t)data);
	dnslib_node_free_rrsets(node, free_rdata_dnames);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Destroys node owner.
 *
 * This function is designed to be used in the tree-iterating functions.
 *
 * \param node Node to destroy the owner of.
 * \param data Unused parameter.
 */
static void dnslib_zone_destroy_node_owner_from_tree(dnslib_node_t *node,
                                                     void *data)
{
	UNUSED(data);
	dnslib_node_free(&node, 1);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts one RDATA item by replacing domain name by one present in the
 *        zone.
 *
 * This function tries to find the domain name in the zone. If the name is not
 * in the zone, it does nothing. If it is there, it destroys the domain name
 * stored in the RDATA item and replaces it by pointer to the domain name from
 * the zone.
 *
 * \warning Call this function only with RDATA items which store domain names,
 *          otherwise the behaviour is undefined.
 *
 * \param rdata RDATA where the item is located.
 * \param zone Zone to which the RDATA belongs.
 * \param pos Position of the RDATA item in the RDATA.
 */
static void dnslib_zone_adjust_rdata_item(dnslib_rdata_t *rdata,
                                          dnslib_zone_t *zone, int pos)
{
	const dnslib_rdata_item_t *dname_item
		= dnslib_rdata_item(rdata, pos);

	if (dname_item != NULL) {
		dnslib_dname_t *dname = dname_item->dname;
		const dnslib_node_t *n = NULL;

		n = dnslib_zone_find_node(zone, dname);

		if (n == NULL) {
			return;
		}

		if (n->owner == dname_item->dname) {
			return;
		}
		debug_dnslib_zone("Replacing dname %s by reference to "
		  "dname %s in zone.\n", dname->name, n->owner->name);

		/*!< \note This will not delete duplicated dnames.
			   Has to be removed as soon as possible !!! */
//		dnslib_rdata_item_set_dname(rdata, pos, n->owner);
		dname->node = n->owner->node;
//		dnslib_dname_free(&dname);
	}
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts all RDATA in the given RRSet by replacing domain names by ones
 *        present in the zone.
 *
 * This function selects the RDATA items containing a domain name (according to
 * RR type descriptor of the RRSet's type and adjusts the item using
 * dnslib_zone_adjust_rdata_item().
 *
 * \param rrset RRSet to adjust RDATA in.
 * \param zone Zone to which the RRSet belongs.
 */
static void dnslib_zone_adjust_rdata_in_rrset(dnslib_rrset_t *rrset,
                                              dnslib_zone_t *zone)
{
	uint16_t type = dnslib_rrset_type(rrset);

	dnslib_rrtype_descriptor_t *desc =
		dnslib_rrtype_descriptor_by_type(type);

	dnslib_rdata_t *rdata_first = dnslib_rrset_get_rdata(rrset);
	dnslib_rdata_t *rdata = rdata_first;

	if (rdata == NULL) {
		return;
	}

	while (rdata->next != rdata_first) {
		for (int i = 0; i < rdata->count; ++i) {
			if (desc->wireformat[i]
			    == DNSLIB_RDATA_WF_COMPRESSED_DNAME
			    || desc->wireformat[i]
			       == DNSLIB_RDATA_WF_UNCOMPRESSED_DNAME
			    || desc->wireformat[i]
			       == DNSLIB_RDATA_WF_LITERAL_DNAME) {
				debug_dnslib_zone("Adjusting domain name at "
				  "position %d of RDATA of record with owner "
				  "%s and type %s.\n",
				  i, rrset->owner->name,
				  dnslib_rrtype_to_string(type));

				dnslib_zone_adjust_rdata_item(rdata, zone, i);
			}
		}
		rdata = rdata->next;
	}

	for (int i = 0; i < rdata->count; ++i) {
		if (desc->wireformat[i]
		    == DNSLIB_RDATA_WF_COMPRESSED_DNAME
		    || desc->wireformat[i]
		       == DNSLIB_RDATA_WF_UNCOMPRESSED_DNAME
		    || desc->wireformat[i]
		       == DNSLIB_RDATA_WF_LITERAL_DNAME) {
			debug_dnslib_zone("Adjusting domain name at "
			  "position %d of RDATA of record with owner "
			  "%s and type %s.\n",
			  i, rrset->owner->name,
			  dnslib_rrtype_to_string(type));

			dnslib_zone_adjust_rdata_item(rdata, zone, i);
		}
	}

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts all RRSets in the given node by replacing domain names in
 *        RDATA by ones present in the zone.
 *
 * This function just calls dnslib_zone_adjust_rdata_in_rrset() for all RRSets
 * in the node (including all RRSIG RRSets).
 *
 * \param node Zone node to adjust the RRSets in.
 * \param zone Zone to which the node belongs.
 */
static void dnslib_zone_adjust_rrsets(dnslib_node_t *node, dnslib_zone_t *zone)
{
	dnslib_rrset_t **rrsets = dnslib_node_get_rrsets(node);
	short count = dnslib_node_rrset_count(node);

	assert(count == 0 || rrsets != NULL);

	for (int r = 0; r < count; ++r) {
		assert(rrsets[r] != NULL);
		dnslib_zone_adjust_rdata_in_rrset(rrsets[r], zone);
		dnslib_rrset_t *rrsigs = rrsets[r]->rrsigs;
		if (rrsigs != NULL) {
			dnslib_zone_adjust_rdata_in_rrset(rrsigs, zone);
		}
	}
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts zone node for faster query processing.
 *
 * - Adjusts RRSets in the node (see dnslib_zone_adjust_rrsets()).
 * - Marks the node as delegation point or non-authoritative (below a zone cut)
 *   if applicable.
 * - Stores reference to corresponding NSEC3 node if applicable.
 *
 * \param node Zone node to adjust.
 * \param zone Zone the node belongs to.
 */
static void dnslib_zone_adjust_node(dnslib_node_t *node, dnslib_zone_t *zone)
{

DEBUG_DNSLIB_ZONE(
	char *name = dnslib_dname_to_str(node->owner);
	debug_dnslib_zone("----- Adjusting node %s -----\n", name);
	free(name);
);

	// adjust domain names in RDATA
	dnslib_zone_adjust_rrsets(node, zone);

DEBUG_DNSLIB_ZONE(
	if (node->parent) {
		char *name = dnslib_dname_to_str(node->parent->owner);
		debug_dnslib_zone("Parent: %s\n", name);
		debug_dnslib_zone("Parent is delegation point: %s\n",
		       dnslib_node_is_deleg_point(node->parent) ? "yes" : "no");
		debug_dnslib_zone("Parent is non-authoritative: %s\n",
		       dnslib_node_is_non_auth(node->parent) ? "yes" : "no");
		free(name);
	} else {
		debug_dnslib_zone("No parent!\n");
	}
);
	// delegation point / non-authoritative node
	if (node->parent
	    && (dnslib_node_is_deleg_point(node->parent)
		|| dnslib_node_is_non_auth(node->parent))) {
		dnslib_node_set_non_auth(node);
	} else if (dnslib_node_rrset(node, DNSLIB_RRTYPE_NS) != NULL
		   && node != zone->apex) {
		dnslib_node_set_deleg_point(node);
	}

	// NSEC3 node
	assert(node->owner);
	const dnslib_node_t *prev;
	int match = dnslib_zone_find_nsec3_for_name(zone, node->owner,
						    &node->nsec3_node, &prev);
	if (match != DNSLIB_ZONE_NAME_FOUND) {
		node->nsec3_node = NULL;
	}

	debug_dnslib_zone("Set flags to the node: \n");
	debug_dnslib_zone("Delegation point: %s\n",
	       dnslib_node_is_deleg_point(node) ? "yes" : "no");
	debug_dnslib_zone("Non-authoritative: %s\n",
	       dnslib_node_is_non_auth(node) ? "yes" : "no");
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts a NSEC3 node for faster query processing.
 *
 * This function just adjusts all RRSets in the node, similarly as the
 * dnslib_zone_adjust_rrsets() function.
 *
 * \param node Zone node to adjust.
 * \param zone Zone the node belongs to.
 */
static void dnslib_zone_adjust_nsec3_node(dnslib_node_t *node,
                                          dnslib_zone_t *zone)
{

DEBUG_DNSLIB_ZONE(
	char *name = dnslib_dname_to_str(node->owner);
	debug_dnslib_zone("----- Adjusting node %s -----\n", name);
	free(name);
);

	// adjust domain names in RDATA
	dnslib_rrset_t **rrsets = dnslib_node_get_rrsets(node);
	short count = dnslib_node_rrset_count(node);

	assert(count == 0 || rrsets != NULL);

	for (int r = 0; r < count; ++r) {
		assert(rrsets[r] != NULL);
		assert(dnslib_rrset_type(rrsets[r]) == DNSLIB_RRTYPE_NSEC3);
		dnslib_rrset_t *rrsigs = rrsets[r]->rrsigs;
		if (rrsigs != NULL) {
			dnslib_zone_adjust_rdata_in_rrset(rrsigs, zone);
		}
	}
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts zone node for faster query processing.
 *
 * This function is just a wrapper over dnslib_zone_adjust_node() to be used
 * in tree-traversing functions.
 *
 * \param node Zone node to adjust.
 * \param data Zone the node belongs to.
 */
static void dnslib_zone_adjust_node_in_tree(dnslib_node_t *node, void *data)
{
	assert(data != NULL);
	dnslib_zone_t *zone = (dnslib_zone_t *)data;

	dnslib_zone_adjust_node(node, zone);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Adjusts NSEC3 node for faster query processing.
 *
 * This function is just a wrapper over dnslib_zone_adjust_nsec3_node() to be
 * used in tree-traversing functions.
 *
 * \param node Zone node to adjust.
 * \param data Zone the node belongs to.
 */
static void dnslib_zone_adjust_nsec3_node_in_tree(dnslib_node_t *node,
                                                  void *data)
{
	assert(data != NULL);
	dnslib_zone_t *zone = (dnslib_zone_t *)data;

	dnslib_zone_adjust_nsec3_node(node, zone);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Creates a NSEC3 hashed name for the given domain name.
 *
 * \note The zone's NSEC3PARAM record must be parsed prior to calling this
 *       function (see dnslib_zone_load_nsec3param()).
 *
 * \param zone Zone from which to take the NSEC3 parameters.
 * \param name Domain name to hash.
 * \param nsec3_name Hashed name.
 *
 * \retval DNSLIB_EOK
 * \retval DNSLIB_ENSEC3PAR
 * \retval DNSLIB_ECRYPTO
 * \retval DNSLIB_ERROR if an error occured while creating a new domain name
 *                      from the hash or concatenating it with the zone name.
 */
static int dnslib_zone_nsec3_name(const dnslib_zone_t *zone,
                                  const dnslib_dname_t *name,
                                  dnslib_dname_t **nsec3_name)
{
	assert(nsec3_name != NULL);

	*nsec3_name = NULL;

	const dnslib_nsec3_params_t *nsec3_params =
		dnslib_zone_nsec3params(zone);

	if (nsec3_params == NULL) {
DEBUG_DNSLIB_ZONE(
		char *n = dnslib_dname_to_str(zone->apex->owner);
		debug_dnslib_zone("No NSEC3PARAM for zone %s.\n", n);
		free(n);
);
		return DNSLIB_ENSEC3PAR;
	}

	uint8_t *hashed_name = NULL;
	size_t hash_size = 0;

DEBUG_DNSLIB_ZONE(
	char *n = dnslib_dname_to_str(name);
	debug_dnslib_zone("Hashing name %s.\n", n);
	free(n);
);

	int res = dnslib_nsec3_sha1(nsec3_params, dnslib_dname_name(name),
	                            dnslib_dname_size(name), &hashed_name,
	                            &hash_size);

	if (res != 0) {
		char *n = dnslib_dname_to_str(name);
		debug_dnslib_zone("Error while hashing name %s.\n", n);
		free(n);
		return DNSLIB_ECRYPTO;
	}

	debug_dnslib_zone("Hash: ");
	debug_dnslib_zone_hex((char *)hashed_name, hash_size);
	debug_dnslib_zone("\n");

	char *name_b32 = NULL;
	size_t size = base32hex_encode_alloc((char *)hashed_name, hash_size,
	                                     &name_b32);

	if (size == 0) {
		char *n = dnslib_dname_to_str(name);
		debug_dnslib_zone("Error while encoding hashed name %s to "
		                  "base32.\n", n);
		free(n);
		if (name_b32 != NULL) {
			free(name_b32);
		}
		return DNSLIB_ECRYPTO;
	}

	assert(name_b32 != NULL);
	free(hashed_name);

	debug_dnslib_zone("Base32-encoded hash: %s\n", name_b32);

	*nsec3_name = dnslib_dname_new_from_str(name_b32, size, NULL);

	free(name_b32);

	if (*nsec3_name == NULL) {
		debug_dnslib_zone("Error while creating domain name for hashed"
		                  " name.\n");
		return DNSLIB_ERROR;
	}

	assert(zone->apex->owner != NULL);
	dnslib_dname_t *ret = dnslib_dname_cat(*nsec3_name, zone->apex->owner);

	if (ret == NULL) {
		debug_dnslib_zone("Error while creating NSEC3 domain name for "
		                  "hashed name.\n");
		dnslib_dname_free(nsec3_name);
		return DNSLIB_ERROR;
	}

	assert(ret == *nsec3_name);

	return DNSLIB_EOK;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Tries to find the given domain name in the zone tree.
 *
 * \param zone Zone to search in.
 * \param name Domain name to find.
 * \param node Found node.
 * \param previous Previous node in canonical order (i.e. the one directly
 *                 preceding \a name in canonical order, regardless if the name
 *                 is in the zone or not).
 *
 * \retval <> 0 if the domain name was found. In such case \a node holds the
 *              zone node with \a name as its owner. \a previous is set
 *              properly.
 * \retval 0 if the domain name was not found. \a node may hold any (or none)
 *           node. \a previous is set properly.
 */
static int dnslib_zone_find_in_tree(const dnslib_zone_t *zone,
                                    const dnslib_dname_t *name,
                                    const dnslib_node_t **node,
                                    const dnslib_node_t **previous)
{
	assert(zone != NULL);
	assert(name != NULL);
	assert(node != NULL);
	assert(previous != NULL);

	dnslib_node_t *found = NULL, *prev = NULL;

	// create dummy node to use for lookup
	dnslib_node_t *tmp = dnslib_node_new((dnslib_dname_t *)name, NULL);
	int exact_match = TREE_FIND_LESS_EQUAL(
	                  zone->tree, dnslib_node, avl, tmp, &found, &prev);
	dnslib_node_free(&tmp, 0);

	*node = found;

	if (prev == NULL) {
		// either the returned node is the root of the tree, or it is
		// the leftmost node in the tree; in both cases node was found
		// set the previous node of the found node
		assert(exact_match);
		assert(found != NULL);
		*previous = dnslib_node_previous(found);
	} else {
		// otherwise check if the previous node is not an empty
		// non-terminal
		*previous = (dnslib_node_rrset_count(prev) == 0)
		            ? dnslib_node_previous(prev)
		            : prev;
	}

	return exact_match;
}

/*----------------------------------------------------------------------------*/
/* API functions                                                              */
/*----------------------------------------------------------------------------*/

dnslib_zone_t *dnslib_zone_new(dnslib_node_t *apex, uint node_count)
{
	if (apex == NULL) {
		return NULL;
	}

	dnslib_zone_t *zone = (dnslib_zone_t *)malloc(sizeof(dnslib_zone_t));
	if (zone == NULL) {
		ERR_ALLOC_FAILED;
		return NULL;
	}

	zone->apex = apex;
	zone->tree = malloc(sizeof(avl_tree_t));
	if (zone->tree == NULL) {
		ERR_ALLOC_FAILED;
		free(zone);
		return NULL;
	}
	zone->nsec3_nodes = malloc(sizeof(avl_tree_t));
	if (zone->nsec3_nodes == NULL) {
		ERR_ALLOC_FAILED;
		free(zone->tree);
		free(zone);
		return NULL;
	}
	zone->node_count = node_count;

	/* Initialize ACLs. */
	zone->acl.xfr_out = 0;
	zone->acl.notify_in = 0;
	zone->acl.notify_out = 0;

	/* Initialize XFR-IN. */
	sockaddr_init(&zone->xfr_in.master, -1);
	zone->xfr_in.timer = 0;
	zone->xfr_in.expire = 0;
	zone->xfr_in.ifaces = 0;

	/* Initialize NSEC3 params */
	zone->nsec3_params.algorithm = 0;
	zone->nsec3_params.flags = 0;
	zone->nsec3_params.iterations = 0;
	zone->nsec3_params.salt_length = 0;
	zone->nsec3_params.salt = NULL;

	TREE_INIT(zone->tree, dnslib_node_compare);
	TREE_INIT(zone->nsec3_nodes, dnslib_node_compare);

	// how to know if this is successfull??
	TREE_INSERT(zone->tree, dnslib_node, avl, apex);

#ifdef USE_HASH_TABLE
	if (zone->node_count > 0) {
		zone->table = ck_create_table(zone->node_count);
		if (zone->table == NULL) {
			free(zone->tree);
			free(zone->nsec3_nodes);
			free(zone);
			return NULL;
		}

		// insert the apex into the hash table
		if (ck_insert_item(zone->table, (const char *)apex->owner->name,
		                   apex->owner->size, (void *)apex) != 0) {
			ck_destroy_table(&zone->table, NULL, 0);
			free(zone->tree);
			free(zone->nsec3_nodes);
			free(zone);
			return NULL;
		}
	} else {
		zone->table = NULL;
	}
#endif
	return zone;
}

/*----------------------------------------------------------------------------*/

int dnslib_zone_add_node(dnslib_zone_t *zone, dnslib_node_t *node)
{
	int ret = 0;
	if ((ret = dnslib_zone_check_node(zone, node)) != 0) {
		return ret;
	}

	// add the node to the tree
	// how to know if this is successfull??
	TREE_INSERT(zone->tree, dnslib_node, avl, node);

#ifdef USE_HASH_TABLE
	//assert(zone->table != NULL);
	// add the node also to the hash table if authoritative, or deleg. point
	if (zone->table != NULL
	    && ck_insert_item(zone->table, (const char *)node->owner->name,
	                      node->owner->size, (void *)node) != 0) {
		debug_dnslib_zone("Error inserting node into hash table!\n");
		return DNSLIB_EHASH;
	}
#endif
	return DNSLIB_EOK;
}

/*----------------------------------------------------------------------------*/

int dnslib_zone_add_nsec3_node(dnslib_zone_t *zone, dnslib_node_t *node)
{
	int ret = 0;
	if ((ret = dnslib_zone_check_node(zone, node)) != 0) {
		return ret;
	}

	// how to know if this is successfull??
	TREE_INSERT(zone->nsec3_nodes, dnslib_node, avl, node);

	return DNSLIB_EOK;
}

/*----------------------------------------------------------------------------*/

dnslib_node_t *dnslib_zone_get_node(const dnslib_zone_t *zone,
				    const dnslib_dname_t *name)
{
	if (zone == NULL || name == NULL) {
		return NULL;
	}

	// create dummy node to use for lookup
	dnslib_node_t *tmp = dnslib_node_new((dnslib_dname_t *)name, NULL);
	dnslib_node_t *n = TREE_FIND(zone->tree, dnslib_node, avl, tmp);
	dnslib_node_free(&tmp, 0);

	return n;
}

/*----------------------------------------------------------------------------*/

dnslib_node_t *dnslib_zone_get_nsec3_node(const dnslib_zone_t *zone,
					  const dnslib_dname_t *name)
{
	if (zone == NULL || name == NULL) {
		return NULL;
	}

	// create dummy node to use for lookup
	dnslib_node_t *tmp = dnslib_node_new((dnslib_dname_t *)name, NULL);
	dnslib_node_t *n = TREE_FIND(zone->nsec3_nodes, dnslib_node, avl, tmp);
	dnslib_node_free(&tmp, 0);

	return n;
}

/*----------------------------------------------------------------------------*/

const dnslib_node_t *dnslib_zone_find_node(const dnslib_zone_t *zone,
					   const dnslib_dname_t *name)
{
	return dnslib_zone_get_node(zone, name);
}

/*----------------------------------------------------------------------------*/

int dnslib_zone_find_dname(const dnslib_zone_t *zone,
                           const dnslib_dname_t *name,
                           const dnslib_node_t **node,
                           const dnslib_node_t **closest_encloser,
                           const dnslib_node_t **previous)
{
	if (zone == NULL || name == NULL || node == NULL
	    || closest_encloser == NULL || previous == NULL) {
		return DNSLIB_EBADARG;
	}

DEBUG_DNSLIB_ZONE(
	char *name_str = dnslib_dname_to_str(name);
	char *zone_str = dnslib_dname_to_str(zone->apex->owner);
	debug_dnslib_zone("Searching for name %s in zone %s...\n",
	                  name_str, zone_str);
	free(name_str);
	free(zone_str);
);

	if (dnslib_dname_compare(name, zone->apex->owner) == 0) {
		*node = zone->apex;
		*closest_encloser = *node;
		return DNSLIB_ZONE_NAME_FOUND;
	}

	if (!dnslib_dname_is_subdomain(name, zone->apex->owner)) {
		*node = NULL;
		*closest_encloser = NULL;
		return DNSLIB_EBADZONE;
	}

	int exact_match = dnslib_zone_find_in_tree(zone, name, node, previous);

DEBUG_DNSLIB_ZONE(
	char *name_str = (*node) ? dnslib_dname_to_str((*node)->owner)
	                         : "(nil)";
	char *name_str2 = (*previous != NULL)
	                  ? dnslib_dname_to_str((*previous)->owner)
	                  : "(nil)";
	debug_dnslib_zone("Search function returned %d, node %s and prev: %s\n",
			  exact_match, name_str, name_str2);

	if (*node) {
		free(name_str);
	}
	if (*previous != NULL) {
		free(name_str2);
	}
);

	*closest_encloser = *node;

	// there must be at least one node with domain name less or equal to
	// the searched name if the name belongs to the zone (the root)
	if (*node == NULL) {
		return DNSLIB_EBADZONE;
	}

	// TODO: this could be replaced by saving pointer to closest encloser
	//       in node

	if (!exact_match) {
		int matched_labels = dnslib_dname_matched_labels(
				(*closest_encloser)->owner, name);
		while (matched_labels
		       < dnslib_dname_label_count((*closest_encloser)->owner)) {
			(*closest_encloser) = (*closest_encloser)->parent;
			assert(*closest_encloser);
		}
	}
DEBUG_DNSLIB_ZONE(
	char *n = dnslib_dname_to_str((*closest_encloser)->owner);
	debug_dnslib_zone("Closest encloser: %s\n", n);
	free(n);
);

	debug_dnslib_zone("find_dname() returning %d\n", exact_match);

	return (exact_match)
	       ? DNSLIB_ZONE_NAME_FOUND
	       : DNSLIB_ZONE_NAME_NOT_FOUND;
}

/*----------------------------------------------------------------------------*/

const dnslib_node_t *dnslib_zone_find_previous(const dnslib_zone_t *zone,
                                               const dnslib_dname_t *name)
{
	if (zone == NULL || name == NULL) {
		return NULL;
	}

	const dnslib_node_t *found = NULL, *prev = NULL;

	(void)dnslib_zone_find_in_tree(zone, name, &found, &prev);
	assert(prev != NULL);

	return prev;
}

/*----------------------------------------------------------------------------*/
#ifdef USE_HASH_TABLE
int dnslib_zone_find_dname_hash(const dnslib_zone_t *zone,
                                const dnslib_dname_t *name,
                                const dnslib_node_t **node,
                                const dnslib_node_t **closest_encloser)
{
	if (zone == NULL || name == NULL || node == NULL
	    || closest_encloser == NULL) {
		return DNSLIB_EBADARG;
	}

DEBUG_DNSLIB_ZONE(
	char *name_str = dnslib_dname_to_str(name);
	char *zone_str = dnslib_dname_to_str(zone->apex->owner);
	debug_dnslib_zone("Searching for name %s in zone %s...\n",
	                  name_str, zone_str);
	free(name_str);
	free(zone_str);
);

	if (dnslib_dname_compare(name, zone->apex->owner) == 0) {
		*node = zone->apex;
		*closest_encloser = *node;
		return DNSLIB_ZONE_NAME_FOUND;
	}

	if (!dnslib_dname_is_subdomain(name, zone->apex->owner)) {
		*node = NULL;
		*closest_encloser = NULL;
		return DNSLIB_EBADZONE;
	}

	const ck_hash_table_item_t *item = ck_find_item(zone->table,
	                                               (const char *)name->name,
	                                               name->size);

	if (item != NULL) {
		*node = (const dnslib_node_t *)item->value;
		*closest_encloser = *node;

		debug_dnslib_zone("Found node in hash table: %p (owner %p, "
		                  "labels: %d)\n", *node, (*node)->owner,
		                  dnslib_dname_label_count((*node)->owner));
		assert(*node != NULL);
		assert(*closest_encloser != NULL);
		return DNSLIB_ZONE_NAME_FOUND;
	}

	*node = NULL;

	// chop leftmost labels until some node is found
	// copy the name for chopping
	dnslib_dname_t *name_copy = dnslib_dname_copy(name);
DEBUG_DNSLIB_ZONE(
	char *n = dnslib_dname_to_str(name_copy);
	debug_dnslib_zone("Finding closest encloser..\nStarting with: %s\n", n);
	free(n);
);

	while (item == NULL) {
		dnslib_dname_left_chop_no_copy(name_copy);
DEBUG_DNSLIB_ZONE(
		char *n = dnslib_dname_to_str(name_copy);
		debug_dnslib_zone("Chopped leftmost label: %s (%.*s, size %u)"
		                  "\n", n, name_copy->size, name_copy->name,
		                  name_copy->size);
		free(n);
);
		// not satisfied in root zone!!
		assert(name_copy->label_count > 0);

		item = ck_find_item(zone->table, (const char *)name_copy->name,
		                    name_copy->size);
	}

	dnslib_dname_free(&name_copy);

	assert(item != NULL);
	*closest_encloser = (const dnslib_node_t *)item->value;

	return DNSLIB_ZONE_NAME_NOT_FOUND;
}
#endif
/*----------------------------------------------------------------------------*/

const dnslib_node_t *dnslib_zone_find_nsec3_node(const dnslib_zone_t *zone,
                                                 const dnslib_dname_t *name)
{
	return dnslib_zone_get_nsec3_node(zone, name);
}

/*----------------------------------------------------------------------------*/

int dnslib_zone_find_nsec3_for_name(const dnslib_zone_t *zone,
                                    const dnslib_dname_t *name,
                                    const dnslib_node_t **nsec3_node,
                                    const dnslib_node_t **nsec3_previous)
{
	if (zone == NULL || name == NULL
	    || nsec3_node == NULL || nsec3_previous == NULL) {
		return DNSLIB_EBADARG;
	}

	dnslib_dname_t *nsec3_name = NULL;
	int ret = dnslib_zone_nsec3_name(zone, name, &nsec3_name);

	if (ret != DNSLIB_EOK) {
		return ret;
	}

DEBUG_DNSLIB_ZONE(
	char *n = dnslib_dname_to_str(nsec3_name);
	debug_dnslib_zone("NSEC3 node name: %s.\n", n);
	free(n);
);

	dnslib_node_t *found = NULL, *prev = NULL;

	// create dummy node to use for lookup
	dnslib_node_t *tmp = dnslib_node_new(nsec3_name, NULL);
	int exact_match = TREE_FIND_LESS_EQUAL(zone->nsec3_nodes, dnslib_node, \
			   avl, tmp, &found, &prev);
	dnslib_node_free(&tmp, 1);

DEBUG_DNSLIB_ZONE(
	if (found) {
		char *n = dnslib_dname_to_str(found->owner);
		debug_dnslib_zone("Found NSEC3 node: %s.\n", n);
		free(n);
	} else {
		debug_dnslib_zone("Found no NSEC3 node.\n");
	}

	if (prev) {
		assert(prev->owner);
		char *n = dnslib_dname_to_str(prev->owner);
		debug_dnslib_zone("Found previous NSEC3 node: %s.\n", n);
		free(n);
	} else {
		debug_dnslib_zone("Found no previous NSEC3 node.\n");
	}
);
	*nsec3_node = found;

	if (prev == NULL) {
		// either the returned node is the root of the tree, or it is
		// the leftmost node in the tree; in both cases node was found
		// set the previous node of the found node
		assert(exact_match);
		assert(*nsec3_node != NULL);
		*nsec3_previous = dnslib_node_previous(*nsec3_node);
	} else {
		*nsec3_previous = prev;
	}

	debug_dnslib_zone("find_nsec3_for_name() returning %d\n", exact_match);

	return (exact_match)
	       ? DNSLIB_ZONE_NAME_FOUND
	       : DNSLIB_ZONE_NAME_NOT_FOUND;
}

/*----------------------------------------------------------------------------*/

const dnslib_node_t *dnslib_zone_apex(const dnslib_zone_t *zone)
{
	return zone->apex;
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_adjust_dnames(dnslib_zone_t *zone)
{
	// load NSEC3PARAM (needed on adjusting function)
	dnslib_zone_load_nsec3param(zone);

	TREE_FORWARD_APPLY(zone->tree, dnslib_node, avl,
	                   dnslib_zone_adjust_node_in_tree, zone);

	TREE_FORWARD_APPLY(zone->nsec3_nodes, dnslib_node, avl,
	                   dnslib_zone_adjust_nsec3_node_in_tree, zone);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_load_nsec3param(dnslib_zone_t *zone)
{
	assert(zone);
	assert(zone->apex);
	const dnslib_rrset_t *rrset = dnslib_node_rrset(zone->apex,
						      DNSLIB_RRTYPE_NSEC3PARAM);

	if (rrset != NULL) {
		dnslib_nsec3_params_from_wire(&zone->nsec3_params, rrset);
	} else {
		memset(&zone->nsec3_params, 0, sizeof(dnslib_nsec3_params_t));
	}
}

/*----------------------------------------------------------------------------*/

int dnslib_zone_nsec3_enabled(const dnslib_zone_t *zone)
{
	return (zone->nsec3_params.algorithm != 0);
}

/*----------------------------------------------------------------------------*/

const dnslib_nsec3_params_t *dnslib_zone_nsec3params(const dnslib_zone_t *zone)
{
	if (dnslib_zone_nsec3_enabled(zone)) {
		return &zone->nsec3_params;
	} else {
		return NULL;
	}
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_tree_apply_postorder(dnslib_zone_t *zone,
                              void (*function)(dnslib_node_t *node, void *data),
                              void *data)
{
	if (zone == NULL) {
		return;
	}

	TREE_POST_ORDER_APPLY(zone->tree, dnslib_node, avl, function, data);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_tree_apply_inorder(dnslib_zone_t *zone,
                              void (*function)(dnslib_node_t *node, void *data),
                              void *data)
{
	if (zone == NULL) {
		return;
	}

	TREE_FORWARD_APPLY(zone->tree, dnslib_node, avl, function, data);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_tree_apply_inorder_reverse(dnslib_zone_t *zone,
                              void (*function)(dnslib_node_t *node, void *data),
                              void *data)
{
	if (zone == NULL) {
		return;
	}

	TREE_REVERSE_APPLY(zone->tree, dnslib_node, avl, function, data);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_nsec3_apply_postorder(dnslib_zone_t *zone,
                              void (*function)(dnslib_node_t *node, void *data),
                              void *data)
{
	if (zone == NULL) {
		return;
	}

	TREE_POST_ORDER_APPLY(zone->nsec3_nodes, dnslib_node, avl, function,
	                      data);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_nsec3_apply_inorder(dnslib_zone_t *zone,
                              void (*function)(dnslib_node_t *node, void *data),
                              void *data)
{
	if (zone == NULL) {
		return;
	}

	TREE_FORWARD_APPLY(zone->nsec3_nodes, dnslib_node, avl, function, data);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_nsec3_apply_inorder_reverse(dnslib_zone_t *zone,
                              void (*function)(dnslib_node_t *node, void *data),
                              void *data)
{
	if (zone == NULL) {
		return;
	}

	TREE_REVERSE_APPLY(zone->nsec3_nodes, dnslib_node, avl, function, data);
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_free(dnslib_zone_t **zone)
{
	if (zone == NULL || *zone == NULL) {
		return;
	}

	free((*zone)->tree);
	free((*zone)->nsec3_nodes);

#ifdef USE_HASH_TABLE
	if ((*zone)->table != NULL) {
		ck_destroy_table(&(*zone)->table, NULL, 0);
	}
#endif

	dnslib_nsec3_params_free(&(*zone)->nsec3_params);

	acl_delete(&(*zone)->acl.xfr_out);
	acl_delete(&(*zone)->acl.notify_in);
	acl_delete(&(*zone)->acl.notify_out);

	free(*zone);
	*zone = NULL;
}

/*----------------------------------------------------------------------------*/

void dnslib_zone_deep_free(dnslib_zone_t **zone, int free_rdata_dnames)
{
	if (zone == NULL || *zone == NULL) {
		return;
	}
#ifdef USE_HASH_TABLE
	if ((*zone)->table != NULL) {
		ck_destroy_table(&(*zone)->table, NULL, 0);
	}
#endif
	/* has to go through zone twice, rdata may contain references to node
	   owners earlier in the zone which may be already freed */
	/* NSEC3 tree is deleted first as it may contain references to the
	   normal tree. */

	TREE_POST_ORDER_APPLY((*zone)->nsec3_nodes, dnslib_node, avl,
	                      dnslib_zone_destroy_node_rrsets_from_tree,
			      (void *)((intptr_t)free_rdata_dnames));

	TREE_POST_ORDER_APPLY((*zone)->nsec3_nodes, dnslib_node, avl,
	                      dnslib_zone_destroy_node_owner_from_tree, NULL);

	TREE_POST_ORDER_APPLY((*zone)->tree, dnslib_node, avl,
	                      dnslib_zone_destroy_node_rrsets_from_tree,
			      (void *)((intptr_t)free_rdata_dnames));

	TREE_POST_ORDER_APPLY((*zone)->tree, dnslib_node, avl,
	                      dnslib_zone_destroy_node_owner_from_tree, NULL);

	dnslib_zone_free(zone);
}
