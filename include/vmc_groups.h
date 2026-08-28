#ifndef VMC_GROUPS_H
#define VMC_GROUPS_H

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Finds the Group ID associated with a given Title ID.
 *
 * This function iterates through all embedded memory card groups.
 * If the provided Title ID is found within any group's list of Title IDs,
 * the Group ID for that group is returned.
 * If the Title ID is not found in any group, the original Title ID
 * passed as a parameter is returned.
 *
 * @param titleId A null-terminated string representing the Title ID to search for.
 * @return A const char* pointer to the Group ID string if found,
 * otherwise, a const char* pointer to the original titleId parameter.
 */
const char *getGroupIdForTitleId(const char *titleId);

#ifdef __cplusplus
}
#endif

#endif // VMC_GROUPS_H

