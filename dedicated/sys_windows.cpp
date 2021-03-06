#include <windows.h> 
#include <stdio.h>
#include <stdlib.h>
#include "isys.h"
#include "conproc.h"
#include "dedicated.h"
#include "exefuncs.h"
#include "launcher_int.h"
#include "dll_state.h"
#include "engine_hlds_api.h"
#include "checksum_md5.h"
#include "ifilesystem.h"
#include "mathlib.h"
#include "tier0/vcrmode.h"
#include "tier0/dbg.h"
#include "vstdlib/strtools.h"
#include "vstdlib/icommandline.h"
#include "idedicatedexports.h"
#include <direct.h>


bool InitInstance( );
void ProcessConsoleInput( void );

static const char *g_pszengine = "engine.dll";
IDedicatedServerAPI *engine = NULL;

static	HANDLE	hinput;
static  HANDLE	houtput;

static char	console_text[256];
static int	console_textlen;


//-----------------------------------------------------------------------------
// Modules...
//-----------------------------------------------------------------------------
static CSysModule *s_hMatSystemModule;	
static CreateInterfaceFn s_MaterialSystemFactory;
static CSysModule *s_hEngineModule = NULL;
static CreateInterfaceFn s_EngineFactory;


//-----------------------------------------------------------------------------
// Implementation of IVCRHelpers.
//-----------------------------------------------------------------------------

class CVCRHelpers : public IVCRHelpers
{
public:
	virtual void	ErrorMessage( const char *pMsg )
	{
		printf( "ERROR: %s\n", pMsg );
	}

	virtual void*	GetMainWindow()
	{
		return 0;
	}
};
static CVCRHelpers g_VCRHelpers;


//-----------------------------------------------------------------------------
// Purpose: Implements OS Specific layer ( loosely )
//-----------------------------------------------------------------------------
class CSys : public ISys
{
public:
	bool		GameInit( CreateInterfaceFn dedicatedFactory );
	void		GameShutdown( void );

	void		Sleep( int msec );
	bool		GetExecutableName( char *out );
	void		ErrorMessage( int level, const char *msg );

	void		WriteStatusText( char *szText );
	void		UpdateStatus( int force );

	long		LoadLibrary( char *lib );
	void		FreeLibrary( long library );

	bool		CreateConsoleWindow( void );
	void		DestroyConsoleWindow( void );
	
	// VXP
	void		PrintRaw( char * pszMsg, int nChars = 0 );
	void		Echo( char * pszMsg, int nChars = 0 );
	
	int			ReceiveNewline( void );
	void		ReceiveBackspace( void );
	void		ReceiveTab( void );
	void		ReceiveStandardChar( const char ch );
	void		ReceiveUpArrow( void );
	void		ReceiveDownArrow( void );
	void		ReceiveLeftArrow( void );
	void		ReceiveRightArrow( void );

	void		ConsoleOutput ( char *string );
	char		*ConsoleInput (void);
	void		Printf(char *fmt, ...);
};

static CSys g_Sys;
ISys *sys = &g_Sys;

#define MAX_CONSOLE_TEXTLEN 256
#define MAX_BUFFER_LINES	30

char	m_szConsoleText[ MAX_CONSOLE_TEXTLEN ];						// console text buffer
int		m_nConsoleTextLen = 0;											// console textbuffer length
int		m_nCursorPosition = 0;											// position in the current input line

// Saved input data when scrolling back through command history
char	m_szSavedConsoleText[ MAX_CONSOLE_TEXTLEN ];				// console text buffer
int		m_nSavedConsoleTextLen = 0;										// console textbuffer length

char	m_aszLineBuffer[ MAX_BUFFER_LINES ][ MAX_CONSOLE_TEXTLEN ];	// command buffer last MAX_BUFFER_LINES commands
int		m_nInputLine = 0;												// Current line being entered
int		m_nBrowseLine = 0;												// current buffer line for up/down arrow
int		m_nTotalLines = 0;	

//memset( m_szConsoleText, 0, sizeof( m_szConsoleText ) );

//memset( m_szSavedConsoleText, 0, sizeof( m_szSavedConsoleText ) );

//memset( m_aszLineBuffer, 0, sizeof( m_aszLineBuffer ) );

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : msec - 
//-----------------------------------------------------------------------------
void CSys::Sleep( int msec )
{
	::Sleep( msec );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *lib - 
// Output : long
//-----------------------------------------------------------------------------
long CSys::LoadLibrary( char *lib )
{
	void *hDll = ::LoadLibrary( lib );
	return (long)hDll;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : library - 
//-----------------------------------------------------------------------------
void CSys::FreeLibrary( long library )
{
	if ( !library )
		return;

	::FreeLibrary( (HMODULE)library );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *out - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSys::GetExecutableName( char *out )
{
	if ( !::GetModuleFileName( ( HINSTANCE )GetModuleHandle( NULL ), out, 256 ) )
	{
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : level - 
//			*msg - 
//-----------------------------------------------------------------------------
void CSys::ErrorMessage( int level, const char *msg )
{
	MessageBox( NULL, msg, "Half-Life", MB_OK );
	PostQuitMessage(0);	
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : force - 
//-----------------------------------------------------------------------------
void CSys::UpdateStatus( int force )
{
	static double tLast = 0.0;
	double	tCurrent;
	char	szPrompt[256];
	int		n, nMax;
	char	szMap[32];
	float	fps;

	float delay = 1.0f;
	if ( !engine )
		return;

	tCurrent = Sys_FloatTime();

	engine->UpdateStatus( &fps, &n, &nMax, szMap );

	if ( !force )
	{
		if ( ( tCurrent - tLast ) < delay )
			return;
	}

	tLast = tCurrent;

	sprintf( szPrompt, "%.1f fps %2i/%2i on map %16s", (float)fps, n, nMax, szMap);

	WriteStatusText( szPrompt );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *string - 
// Output : void CSys::ConsoleOutput
//-----------------------------------------------------------------------------
void CSys::ConsoleOutput (char *string)
{
	unsigned long dummy;
	char	text[256];

	if (console_textlen)
	{
		text[0] = '\r';
		memset(&text[1], ' ', console_textlen);
		text[console_textlen+1] = '\r';
		text[console_textlen+2] = 0;
		WriteFile(houtput, text, console_textlen+2, &dummy, NULL);
	}

	WriteFile(houtput, string, strlen(string), &dummy, NULL);

	if (console_textlen)
	{
		WriteFile(houtput, console_text, console_textlen, &dummy, NULL);
	}
	UpdateStatus( 1 /* force */ );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *fmt - 
//			... - 
//-----------------------------------------------------------------------------
void CSys::Printf(char *fmt, ...)
{
	// Dump text to debugging console.
	va_list argptr;
	char szText[1024];

	va_start (argptr, fmt);
	vsprintf (szText, fmt, argptr);
//	Q_vsnprintf (szText, sizeof( szText ), fmt, argptr);
	va_end (argptr);

	// Get Current text and append it.
	ConsoleOutput( szText );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : char *
//-----------------------------------------------------------------------------
/*
char *CSys::ConsoleInput (void)
{
	INPUT_RECORD	recs[1024];
	unsigned long	dummy;
	int				ch;
	unsigned long	numread, numevents;

	while ( 1 )
	{
		if( !g_pVCR->Hook_GetNumberOfConsoleInputEvents( hinput, &numevents ) )
		{
			exit( -1 );
		}

		if (numevents <= 0)
			break;

		if ( !g_pVCR->Hook_ReadConsoleInput(hinput, recs, 1, &numread) )
		{
			exit( -1 );
		}

		if (numread != 1)
		{
			exit( -1 );
		}

		if ( recs[0].EventType == KEY_EVENT )
		{
			if ( !recs[0].Event.KeyEvent.bKeyDown )
			{
				ch = recs[0].Event.KeyEvent.uChar.AsciiChar;
				switch (ch)
				{
					case '\r':
						WriteFile(houtput, "\r\n", 2, &dummy, NULL);	
						if (console_textlen)
						{
							console_text[console_textlen] = 0;
							console_textlen = 0;
							return console_text;
						}
						break;

					case '\b':
						if (console_textlen)
						{
							console_textlen--;
							WriteFile(houtput, "\b \b", 3, &dummy, NULL);	
						}
						break;

					default:
						if (ch >= ' ')
						{
							if (console_textlen < sizeof(console_text)-2)
							{
								WriteFile(houtput, &ch, 1, &dummy, NULL);	
								console_text[console_textlen] = ch;
								console_textlen++;
							}
						}

						break;

				}
			}
		}
	}

	return NULL;
}
*/

void CSys::PrintRaw( char * pszMsg, int nChars )
{
	unsigned long dummy;

	if ( nChars == 0 )
	{
		WriteFile( houtput, pszMsg, strlen( pszMsg ), &dummy, NULL );
	}
	else
	{
		WriteFile( houtput, pszMsg, nChars, &dummy, NULL );
	}
}

void CSys::Echo( char * pszMsg, int nChars )
{
	PrintRaw( pszMsg, nChars );
/*
	CONSOLE_CURSOR_INFO cinfo = {0};
	GetConsoleCursorInfo(houtput, &cinfo);
	cinfo.dwSize = 100;
	cinfo.bVisible = true;
	SetConsoleCursorInfo(houtput, &cinfo);
*/
}

int CSys::ReceiveNewline( void )
{
	int nLen = 0;

	Echo( "\n" );

	if ( m_nConsoleTextLen )
	{
		nLen = m_nConsoleTextLen;

		m_szConsoleText[ m_nConsoleTextLen ] = 0;
		m_nConsoleTextLen = 0;
		m_nCursorPosition = 0;

		// cache line in buffer, but only if it's not a duplicate of the previous line
		if ( ( m_nInputLine == 0 ) || ( strcmp( m_aszLineBuffer[ m_nInputLine - 1 ], m_szConsoleText ) ) )
		{
			strncpy( m_aszLineBuffer[ m_nInputLine ], m_szConsoleText, MAX_CONSOLE_TEXTLEN );
			
			m_nInputLine++;

			if ( m_nInputLine > m_nTotalLines )
				m_nTotalLines = m_nInputLine;

			if ( m_nInputLine >= MAX_BUFFER_LINES )
				m_nInputLine = 0;

		}

		m_nBrowseLine = m_nInputLine;
	}

	return nLen;
}


void CSys::ReceiveBackspace( void )
{
	int nCount;

	if ( m_nCursorPosition == 0 )
	{
		return;
	}

	m_nConsoleTextLen--;
	m_nCursorPosition--;

	Echo( "\b" );

	for ( nCount = m_nCursorPosition; nCount < m_nConsoleTextLen; nCount++ )
	{
		m_szConsoleText[ nCount ] = m_szConsoleText[ nCount + 1 ];
		Echo( m_szConsoleText + nCount, 1 );
	}

	Echo( " " );

	nCount = m_nConsoleTextLen;
	while ( nCount >= m_nCursorPosition )
	{
		Echo( "\b" );
		nCount--;
	}

	m_nBrowseLine = m_nInputLine;
}

void CSys::ReceiveStandardChar( const char ch )
{
	int nCount;

	// If the line buffer is maxed out, ignore this char
	if ( m_nConsoleTextLen >= ( sizeof( m_szConsoleText ) - 2 ) )
	{
		return;
	}

	nCount = m_nConsoleTextLen;
	while ( nCount > m_nCursorPosition )
	{
		m_szConsoleText[ nCount ] = m_szConsoleText[ nCount - 1 ];
		nCount--;
	}

	m_szConsoleText[ m_nCursorPosition ] = ch;

	Echo( m_szConsoleText + m_nCursorPosition, m_nConsoleTextLen - m_nCursorPosition + 1 );

	m_nConsoleTextLen++;
	m_nCursorPosition++;

	nCount = m_nConsoleTextLen;
	while ( nCount > m_nCursorPosition )
	{
		Echo( "\b" );
		nCount--;
	}

	m_nBrowseLine = m_nInputLine;
}

void CSys::ReceiveUpArrow( void )
{
	int nLastCommandInHistory;

	nLastCommandInHistory = m_nInputLine + 1;
	if ( nLastCommandInHistory > m_nTotalLines )
	{
		nLastCommandInHistory = 0;
	}

	if ( m_nBrowseLine == nLastCommandInHistory )
	{
		return;
	}

	if ( m_nBrowseLine == m_nInputLine )
	{
		if ( m_nConsoleTextLen > 0 )
		{
			// Save off current text
			strncpy( m_szSavedConsoleText, m_szConsoleText, m_nConsoleTextLen );
			// No terminator, it's a raw buffer we always know the length of
		}

		m_nSavedConsoleTextLen = m_nConsoleTextLen;
	}

	m_nBrowseLine--;
	if ( m_nBrowseLine < 0 )
	{
		m_nBrowseLine = m_nTotalLines - 1;
	}

	while ( m_nConsoleTextLen-- )	// delete old line
	{
		Echo( "\b \b" );
	}

	// copy buffered line
	Echo( m_aszLineBuffer[ m_nBrowseLine ] );

	strncpy( m_szConsoleText, m_aszLineBuffer[ m_nBrowseLine ], MAX_CONSOLE_TEXTLEN );
	m_nConsoleTextLen = strlen( m_aszLineBuffer[ m_nBrowseLine ] );

	m_nCursorPosition = m_nConsoleTextLen;
}


void CSys::ReceiveDownArrow( void )
{
	if ( m_nBrowseLine == m_nInputLine )
	{
		return;
	}

	m_nBrowseLine++;
	if ( m_nBrowseLine > m_nTotalLines )
	{
		m_nBrowseLine = 0;
	}

	while ( m_nConsoleTextLen-- )	// delete old line
	{
		Echo( "\b \b" );
	}

	if ( m_nBrowseLine == m_nInputLine )
	{
		if ( m_nSavedConsoleTextLen > 0 )
		{
			// Restore current text
			strncpy( m_szConsoleText, m_szSavedConsoleText, m_nSavedConsoleTextLen );
			// No terminator, it's a raw buffer we always know the length of

			Echo( m_szConsoleText, m_nSavedConsoleTextLen );
		}

		m_nConsoleTextLen = m_nSavedConsoleTextLen;
	}
	else
	{
		// copy buffered line
		Echo( m_aszLineBuffer[ m_nBrowseLine ] );

		strncpy( m_szConsoleText, m_aszLineBuffer[ m_nBrowseLine ], MAX_CONSOLE_TEXTLEN );

		m_nConsoleTextLen = strlen( m_aszLineBuffer[ m_nBrowseLine ] );
	}

	m_nCursorPosition = m_nConsoleTextLen;
}


void CSys::ReceiveLeftArrow( void )
{
	if ( m_nCursorPosition == 0 )
	{
		return;
	}

	Echo( "\b" );
	m_nCursorPosition--;
}


void CSys::ReceiveRightArrow( void )
{
	if ( m_nCursorPosition == m_nConsoleTextLen )
	{
		return;
	}

	Echo( m_szConsoleText + m_nCursorPosition, 1 );
	m_nCursorPosition++;
}
/*
char *CSys::ConsoleInput (void)
{
	while ( 1 )
	{
		INPUT_RECORD	recs[1024];
		unsigned long	dummy;
		unsigned long	numread, numevents;
		if( !g_pVCR->Hook_GetNumberOfConsoleInputEvents( hinput, &numevents ) )
			return NULL;

		if (numevents <= 0)
			break;

		if ( !g_pVCR->Hook_ReadConsoleInput(hinput, recs, 1, &numread) )
			return NULL;

		if (numread == 0)
			return NULL;
			
		for ( int i=0; i < (int)numread; i++ )
		{
			INPUT_RECORD *pRec = &recs[i];
			if ( pRec->EventType != KEY_EVENT )
				continue;

		//	if ( recs[0].EventType == KEY_EVENT )
		//	{
			//	if ( !recs[0].Event.KeyEvent.bKeyDown )
				if ( pRec->Event.KeyEvent.bKeyDown )
				{
					// check for cursor keys
					if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_UP )
					{
						ReceiveUpArrow();
					}
					else if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_DOWN )
					{
						ReceiveDownArrow();
					}
					else if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_LEFT )
					{
						ReceiveLeftArrow();
					}
					else if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_RIGHT )
					{
						ReceiveRightArrow();
					}
					else
					{
					//	ch = recs[0].Event.KeyEvent.uChar.AsciiChar;
						char	ch;
					//	int		nLen;
						ch = pRec->Event.KeyEvent.uChar.AsciiChar;
						switch (ch)
						{
							case '\r':
								WriteFile(houtput, "\r\n", 2, &dummy, NULL);	
								if (console_textlen)
								{
									console_text[console_textlen] = 0;
									console_textlen = 0;
									return console_text;
								}
								break;

							case '\b':
								if (console_textlen)
								{
									console_textlen--;
									WriteFile(houtput, "\b \b", 3, &dummy, NULL);	
								}
								break;

							default:
							//	if (ch >= ' ')
								if ( ( ch >= ' ') && ( ch <= '~' ) )	// dont' accept nonprintable chars
								{
									if (console_textlen < sizeof(console_text)-2)
									{
										WriteFile(houtput, &ch, 1, &dummy, NULL);	
										console_text[console_textlen] = ch;
										console_textlen++;
									}
								}

								break;

						}
					}
				}
		//	}
		}
	}

	return NULL;
}
*/
char * CSys::ConsoleInput( void )
{
	while ( 1 )
	{
		INPUT_RECORD	recs[ 1024 ];
		unsigned long	numread;
		unsigned long	numevents;

		if ( !GetNumberOfConsoleInputEvents( hinput, &numevents ) )
		{
			Error("CTextConsoleWin32::GetLine: !GetNumberOfConsoleInputEvents");

			return NULL;
		}

		if ( numevents <= 0 )
			break;

		if ( !ReadConsoleInput( hinput, recs, ARRAYSIZE( recs ), &numread ) )
		{
			Error("CTextConsoleWin32::GetLine: !ReadConsoleInput");

			return NULL;
		}

		if ( numread == 0 )
			return NULL;

		for ( int i=0; i < (int)numread; i++ )
		{
			INPUT_RECORD *pRec = &recs[i];
			if ( pRec->EventType != KEY_EVENT )
				continue;

			if ( pRec->Event.KeyEvent.bKeyDown )
			{
				// check for cursor keys
				if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_UP )
				{
					ReceiveUpArrow();
				}
				else if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_DOWN )
				{
					ReceiveDownArrow();
				}
				else if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_LEFT )
				{
					ReceiveLeftArrow();
				}
				else if ( pRec->Event.KeyEvent.wVirtualKeyCode == VK_RIGHT )
				{
					ReceiveRightArrow();
				}
				else
				{
					char	ch;
					int		nLen;

					ch = pRec->Event.KeyEvent.uChar.AsciiChar;
					switch ( ch )
					{
					case '\r':	// Enter
						nLen = ReceiveNewline();
						if ( nLen )
						{
							return m_szConsoleText;	
						}
						break;

					case '\b':	// Backspace
						ReceiveBackspace();
						break;

				//	case '\t':	// TAB
				//		ReceiveTab();
				//		break;

					default:
						if ( ( ch >= ' ') && ( ch <= '~' ) )	// dont' accept nonprintable chars
						{
							ReceiveStandardChar( ch );
						}
						break;
					}
				}
			}
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *szText - 
//-----------------------------------------------------------------------------
void CSys::WriteStatusText( char *szText )
{
	SetConsoleTitle( szText );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSys::CreateConsoleWindow( void )
{
	if ( !AllocConsole () )
	{
		return false;
	}

	hinput	= GetStdHandle (STD_INPUT_HANDLE);
	houtput = GetStdHandle (STD_OUTPUT_HANDLE);
	
	InitConProc();
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CSys::DestroyConsoleWindow( void )
{
	FreeConsole ();

	// shut down QHOST hooks if necessary
	DeinitConProc ();
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CSys::GameInit( CreateInterfaceFn dedicatedFactory )
{
	// Check that we are running on Win32
	OSVERSIONINFO	vinfo;
	vinfo.dwOSVersionInfoSize = sizeof(vinfo);

	if ( !GetVersionEx ( &vinfo ) )
	{
		return false;
	}

	if ( vinfo.dwPlatformId == VER_PLATFORM_WIN32s )
	{
		return false;
	}
	
	// Load up the material system
	s_hMatSystemModule = FileSystem_LoadModule("materialsystem.dll");
	if (!s_hMatSystemModule)
		return false;

	s_MaterialSystemFactory = Sys_GetFactory(s_hMatSystemModule);
	if (!s_MaterialSystemFactory)
		return false;

	s_hEngineModule = FileSystem_LoadModule( g_pszengine );
	if ( !s_hEngineModule )
	{
		return false;
	}

	s_EngineFactory = Sys_GetFactory( s_hEngineModule );
	if ( !s_EngineFactory )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CSys::GameShutdown( void )
{
	if ( s_hEngineModule )
	{
		FileSystem_UnloadModule( s_hEngineModule );
	}

	if (s_hMatSystemModule)
		FileSystem_UnloadModule(s_hMatSystemModule);
}



//-----------------------------------------------------------------------------
// An aggregate class factory encompassing all factories in all loaded DLLs
//-----------------------------------------------------------------------------
void* DedicatedFactory(const char *pName, int *pReturnCode)
{
	void *pRetVal = NULL;

	// FIXME: Really, we should just load the DLLs in order
	// and search in the order loaded. This is sort of a half-measure hack
	// to get to where we need to go

	// First ask ourselves
	pRetVal = Sys_GetFactoryThis()( pName, pReturnCode );
	if (pRetVal)
		return pRetVal;

	// First ask the file system...
	pRetVal = GetFileSystemFactory()( pName, pReturnCode );
	if (pRetVal)
		return pRetVal;

	// Next the material system...
	pRetVal = s_MaterialSystemFactory( pName, pReturnCode );
	if (pRetVal)
		return pRetVal;

	// Then the engine
	pRetVal = s_EngineFactory( pName, pReturnCode );
	if (pRetVal)
		return pRetVal;

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool NET_Init( void )
{
	// Startup winock
	WORD version = MAKEWORD( 1, 1 );
	WSADATA wsaData;

	int err = WSAStartup( version, &wsaData );
	if ( err != 0 )
	{
		char msg[ 256 ];
		sprintf( msg, "Winsock 1.1 unavailable...\n" );
		sys->Printf( msg );
		OutputDebugString( msg );
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void NET_Shutdown( void )
{
	// Kill winsock
	WSACleanup();
}

int Sys_GetExecutableName( char *out )
{
#ifdef _WIN32
	if ( !::GetModuleFileName( ( HINSTANCE )GetModuleHandle( NULL ), out, 256 ) )
	{
		return 0;
	}
#else
	strcpy( out, g_szEXEName );
#endif
	return 1;
}


//-----------------------------------------------------------------------------
// Purpose: Return the directory where this .exe is running from
// Output : char
//-----------------------------------------------------------------------------
static const char *UTIL_GetExecutableDir( )
{
	static char	exedir[ MAX_PATH ];

	exedir[ 0 ] = 0;
	if ( !Sys_GetExecutableName( exedir ) )
		return NULL;

	char *pSlash;
	char *pSlash2;
	pSlash = strrchr( exedir,'\\' );
	pSlash2 = strrchr( exedir,'/' );
	if ( pSlash2 > pSlash )
	{
		pSlash = pSlash2;
	}
	if (pSlash)
	{
		*pSlash = 0;
	}

	return exedir;
}


//-----------------------------------------------------------------------------
// Purpose: Return the directory where this .exe is running from
// Output : char
//-----------------------------------------------------------------------------
const char *UTIL_GetBaseDir( void )
{
	static char	basedir[ MAX_PATH ];

	char const *pOverrideDir = CommandLine()->CheckParm( "-basedir" );
	if ( pOverrideDir )
		return pOverrideDir;

	basedir[ 0 ] = 0;
	const char *pExeDir = UTIL_GetExecutableDir( );
	if ( pExeDir )
	{
		strcpy( basedir, pExeDir );
		char *p = Q_stristr( basedir, "bin" );
		if ( p && ( p - basedir ) == (int)(strlen( basedir ) - 3 ) )
		{
			--p;
			*p = 0;
		}
	}

	return basedir;
}



class CDedicatedExports : public IDedicatedExports
{
public:
	void Sys_Printf( char *text )
	{
		sys->Printf( text );
	}
};

EXPOSE_SINGLE_INTERFACE( CDedicatedExports, IDedicatedExports, VENGINE_DEDICATEDEXPORTS_API_VERSION );


SpewRetval_t DedicatedSpewOutputFunc( SpewType_t spewType, char const *pMsg )
{
	printf( "%s", pMsg );
	if( spewType == SPEW_ASSERT )
	{
		if ( CommandLine()->FindParm( "-noassert" ) == 0 )
			return SPEW_DEBUGGER;
		else
			return SPEW_CONTINUE;
	}
	else if( spewType == SPEW_ERROR )
	{
		MessageBox( NULL, pMsg, "dedicated", MB_OK );
		return SPEW_ABORT;
	}
	else
	{
		return SPEW_CONTINUE;
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : hInstance - 
//			hPrevInstance - 
//			lpszCmdLine - 
//			nCmdShow - 
// Output : int PASCAL
//-----------------------------------------------------------------------------
int PASCAL WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdLine, int nCmdShow )
{
	int bDone = 0;
	int		iret = -1;
	MathLib_Init( 2.2f, 2.2f, 0.0f, 1.0f );

	// Hook the debug output stuff.
	SpewOutputFunc( DedicatedSpewOutputFunc );

	// Store off command line for argument searching
	CommandLine()->CreateCmdLine( g_pVCR->Hook_GetCommandLine() );

	// Start VCR mode?
	const char *filename;
	if( CommandLine()->CheckParm( "-vcrrecord", &filename ) )
	{
		if ( !g_pVCR->Start( filename, true, &g_VCRHelpers ) )
		{
			Error( "-vcrrecord: can't open '%s' for writing.\n", filename );
			return -1;
		}
	}
	else if( CommandLine()->CheckParm( "-vcrplayback", &filename ) )
	{
		if ( !g_pVCR->Start( filename, false, &g_VCRHelpers ) )
		{
			Error( "-vcrplayback: can't open '%s' for reading.\n", filename );
			return -1;
		}
	}

	// Rehook the command line through VCR mode.
	CommandLine()->CreateCmdLine( g_pVCR->Hook_GetCommandLine() );

	// Required for DLL loading to happen
	// But actually, we're fucked if this hasn't already happened, since 
	// we've got statically linked DLLs...
	const char *pExeDir = UTIL_GetExecutableDir();
	_chdir( pExeDir );

	// Set default game directory
	const char *pGameDir = NULL;
	CommandLine()->CheckParm( "-game", &pGameDir );
	if ( !CommandLine()->CheckParm( "-defaultgamedir" ) )
	{
		CommandLine()->AppendParm( "-defaultgamedir", "hl2" );
	}

	if ( !InitInstance( ) )
	{
		goto cleanup;
	}

	if ( !sys->CreateConsoleWindow() )
	{
		goto cleanup;
	}

	if ( !NET_Init() )
	{
		goto cleanup;
	}

	if ( !sys->GameInit(DedicatedFactory) )
	{
		goto cleanup;
	}

	engine = (IDedicatedServerAPI *)DedicatedFactory( VENGINE_HLDS_API_VERSION, NULL );
	if ( !engine )
	{
		goto cleanup;
	}

	if ( !engine->Init( UTIL_GetBaseDir(), DedicatedFactory ) )
	{
		goto cleanup;
	}

	while ( 1 )
	{
		if ( bDone )
			break;

#if defined ( _WIN32 )
		MSG msg;

		{
			// Running really fast, yield some time to other apps
			if ( g_pVCR->GetMode() != VCR_Playback )
			{
				Sleep( 1 );
			}
		}
		
		while( g_pVCR->Hook_PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) )
		{
			//if (!GetMessage( &msg, NULL, 0, 0))
			if ( msg.message == WM_QUIT )
			{
				bDone = true;
				break;
			}

			TranslateMessage( &msg );
			DispatchMessage( &msg );
		}

		if ( bDone /*|| gbAppHasBeenTerminated*/ )
			break;
#endif // _WIN32

		ProcessConsoleInput();

		if ( !engine->RunFrame() )
		{
			bDone = true;
		}

		sys->UpdateStatus( 0  /* don't force */ );
	}

	engine->Shutdown();
	
	sys->GameShutdown();

	NET_Shutdown();

	sys->DestroyConsoleWindow();

	iret = 1;

cleanup:
	// Shut down the file system
	FileSystem_Shutdown();

	return iret;
}
