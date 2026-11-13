#include <ctype.h>
#include <iostream>
#include <fstream>
#include <string>

#include <string.h>	/* strcmp() */
#include <stdlib.h>	/* exit() */
#include "pri.h"
#include "gts_master.h"
#include "gts_gui.h"
#include "memory_desktop.h"

#if defined(HAVE_CONFIG_H)
#  include <config.h>
#endif

static void print_version( char *title )
{
pri_funct_msg_vr( "%s  %s  %s",
	title,
	cl_gts_master.cp_release_date(),
	cl_gts_master.cp_release_number()
);
}

static void print_help( char *title, gts_master *clp_master )
{
pri_funct_msg_vr( "Usage : %s [option]", title );
pri_funct_msg_vr( "Option");
pri_funct_msg_vr( " -version : print VERSION");
pri_funct_msg_vr( " -help    : print HELP");
pri_funct_msg_vr( " -bv      : Basic Verbose on");
pri_funct_msg_vr( " -cv      : Counter Verbose on");
pri_funct_msg_vr( " -api-port N   : start the REST API server on port N");
pri_funct_msg_vr( " -api-host H   : REST API bind address (default 127.0.0.1)");
pri_funct_msg_vr( " -api-token T  : REST API requires 'Authorization: Bearer T'");
pri_funct_msg_vr( " -headless     : no windows (needs -api-port)");
pri_funct_msg_vr( "File Format");
pri_funct_msg_vr( " TIFF          (8,16bits)(.tif)" );
pri_funct_msg_vr( " Targa            (8bits)(.tga,.TGA)" );
}

static int argument_analyzer( int argc, char *argv[], char *cp_comm, gts_master *clp_master )
{
	int	ii;

	for (ii = 1; ii < argc; ++ii) {
		if      (!strcmp( argv[ii], "-version" )) {
			print_version( cp_comm ); exit (0);
		}
		else if (!strcmp( argv[ii], "-help"    )) {
			print_help( cp_comm, clp_master ); exit (0);
		}
		else if (!strcmp( argv[ii], "-bv" )) {
			clp_master->i_pv_sw( ON );
			clp_master->i_mv_sw( ON );
		}
		else if (!strcmp( argv[ii], "-cv" )) {
			clp_master->i_cv_sw( ON );
		}
		else if (!strcmp( argv[ii], "-api-port" )
		||       !strcmp( argv[ii], "-api-host" )
		||       !strcmp( argv[ii], "-api-token" )) {
			if (ii + 1 >= argc) {
				pri_funct_err_bttvr(
					"Error: option<%s> needs a value", argv[ii] );
				return NG;
			}
			const char *opt = argv[ii];
			const char *val = argv[++ii];
			if (!strcmp( opt, "-api-port" )) {
				char *end = nullptr;
				const long port = strtol( val, &end, 10 );
				if (*end != '\0' || port < 1 || 65535 < port) {
					pri_funct_err_bttvr(
						"Error: bad port<%s>", val );
					return NG;
				}
				clp_master->api_port( (int)port );
			}
			else if (!strcmp( opt, "-api-host" )) {
				clp_master->api_host( val );
			}
			else {
				clp_master->api_token( val );
			}
		}
		else if (!strcmp( argv[ii], "-headless" )) {
			clp_master->headless( true );
		}

		else if ('-' == argv[ii][0]) {
			pri_funct_err_bttvr(
				"Error: bad option<%s>", argv[ii]  );
			print_help( cp_comm, clp_master );
			return NG;
		}
		else {
			pri_funct_err_bttvr(
				"Error: bad argument<%s>",argv[ii] );
			print_help( cp_comm, clp_master );
			return NG;
		}
	}
	if (clp_master->headless() && clp_master->api_port() <= 0) {
		pri_funct_err_bttvr( "Error: -headless needs -api-port" );
		return NG;
	}
	return OK;
}

/* リリース名、版、日付 */
#ifdef PACKAGE_NAME
gts_master cl_gts_master(PACKAGE_NAME, PACKAGE_VERSION, CONFIGURATION_DATE);
# else
gts_master cl_gts_master( "gts" ,"2.5.1" ,"2019-04-22" );
# endif

int main( int argc, char **argv )
{
	/* 以後のプリント表示にコマンド名を付加する */
	pri_funct_set_cp_title( (char *)cl_gts_master.cp_release_name() );

	/* 初期化設定を設定ファイルから行なう */
	//setup_gts_( argv[0] );
	//cl_gts_master.cl_memo_install_setup.load( argv[0] );

	/* 引数による設定や実行 */
	if (OK != argument_analyzer(
	argc, argv,
	(char *)cl_gts_master.cp_release_name(), &cl_gts_master )) {
		pri_funct_err_bttvr(
	 "Error : argument_analyzer(-) returns NG"  );
		return 2;
	}

	/* 全実行 */
	return cl_gts_master.exec( argv[0] );
}

