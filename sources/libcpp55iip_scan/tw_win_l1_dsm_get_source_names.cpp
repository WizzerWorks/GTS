#include <string.h>
#include "pri.h"
#include "tw_win_l1_dsm.h"

/* Names of all installed TWAIN sources (scanner drivers).
	Opens the Source Manager when needed (needs set_hw_parent() first)
	and returns to the state it was called in. */
int tw_win_l1_dsm::get_source_names( std::vector<std::string>& names )
{
	names.clear();

	const TWAIN_STATE state_before = this->twain_state;

	if (OK != this->state1to2()) { return NG; }
	if (OK != this->state2to3()) {
		if (TWAIN_STATE1_PRE_SESSION == state_before) {
			(void)this->state2to1();
		}
		return NG;
	}

	/* DG_CONTROL/DAT_IDENTITY/MSG_GETFIRST,MSG_GETNEXT are valid in
	state 3 and above. TWRC_ENDOFLIST ends the list. */
	TW_IDENTITY ident;
	memset( &ident, 0, sizeof(ident) );
	TW_UINT16 rc = this->_dsm_entry(
		DG_CONTROL, DAT_IDENTITY, MSG_GETFIRST, (TW_MEMREF)&ident );
	while (TWRC_SUCCESS == rc) {
		names.push_back( std::string( ident.ProductName ) );
		rc = this->_dsm_entry(
			DG_CONTROL, DAT_IDENTITY, MSG_GETNEXT, (TW_MEMREF)&ident );
	}
	const bool ok = (TWRC_ENDOFLIST == rc);

	/* back to the state before this call */
	if (TWAIN_STATE1_PRE_SESSION == state_before
	||  TWAIN_STATE2_SOURCE_MANAGER_LOADED == state_before) {
		(void)this->state3to2();
		if (TWAIN_STATE1_PRE_SESSION == state_before) {
			(void)this->state2to1();
		}
	}
	return ok ? OK : NG;
}
