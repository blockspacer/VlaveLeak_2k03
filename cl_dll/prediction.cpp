//========= Copyright � 1996-2001, Valve LLC, All rights reserved. ============
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================
#include "cbase.h"
#include "prediction.h"
#include "igamemovement.h"
#include "client_command.h"
#include "prediction_private.h"
#include "ivrenderview.h"
#include "iinput.h"
#include "usercmd.h"
#include "imessagechars.h"
#include <vgui_controls/Controls.h>
#include <vgui/ISurface.h>
#include <vgui/IScheme.h>
#include "hud.h"
#include "IClientVehicle.h"
#include "in_buttons.h"
#include "../common/con_nprint.h"
#include "hud_pdump.h"
#include "tier0/vprof.h"


IPredictionSystem *IPredictionSystem::g_pPredictionSystems = NULL;

ConVar	cl_predict			( "cl_predict","1", 0, "Perform client side prediction." );
ConVar	cl_predictweapons	( "cl_predictweapons","0", 0, "Perform client side prediction of weapon effects." );
ConVar	cl_lagcompensation	( "cl_lagcompensation","0", 0, "Perform server side lag compensation of weapon firing events." );
ConVar	cl_showerror		( "cl_showerror", "0", 0, "Show prediction errors, 2 for above plus detailed field deltas." );
static ConVar	cl_smooth			( "cl_smooth", "1", 0, "Smooth view/eye origin after prediction errors" );
static ConVar	cl_smoothtime		( "cl_smoothtime", "0.1", 0, "Smooth client's view after prediction error over this many seconds" );
static ConVar	cl_idealpitchscale	( "cl_idealpitchscale", "0.8", FCVAR_ARCHIVE );
static ConVar	cl_predictionlist( "cl_predictionlist", "0", 0, "Show which entities are predicting\n" );

static ConVar	cl_predictionentitydump( "cl_pdump", "-1", 0, "Dump info about this entity to screen." );
static ConVar	cl_predictionentitydumpbyclass( "cl_pclass", "", 0, "Dump entity by prediction classname." );
static ConVar	cl_pred_optimize( "cl_pred_optimize", "2", 0, "Optimize for not copying data if didn't receive a network update (1), and also for not repredicting if there were no errors (2)." );

extern IGameMovement *g_pGameMovement;
extern CMoveData *g_pMoveData;

void COM_Log( char *pszFile, char *fmt, ...);
typedescription_t *FindFieldByName( const char *fieldname, datamap_t *dmap );

//-----------------------------------------------------------------------------
// Purpose: For debugging, find predictable by classname
// Input  : *classname - 
// Output : static C_BaseEntity
//-----------------------------------------------------------------------------
static C_BaseEntity *FindPredictableByGameClass( const char *classname )
{
	// Walk backward due to deletion from UtlVector
	int c = predictables->GetPredictableCount();
	int i;
	for ( i = 0; i < c; i++ )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		// Don't do anything to truly predicted things (like player and weapons )
		if ( !FClassnameIs( ent, classname ) )
			continue;

		return ent;
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CPrediction::CPrediction( void )
{
	m_flCorrectionTime		= 0.0;

	m_bInPrediction = false;
	m_bFirstTimePredicted = false;

	m_vecPredictionError.Init();
	m_vecLastPredictedOrigin.Init();
	m_nLastPredictedMoveParent = INVALID_EHANDLE_INDEX;

	m_nIncomingPacketNumber = 0;
	m_flIdealPitch = 0.0f;

	m_vecCurrentNetworkOrigin.Init();

	m_nPreviousStartFrame = -1;

	m_nCommandsPredicted = 0;
	m_nServerCommandsAcknowledged = 0;
	m_bPreviousAckHadErrors = false;
}

CPrediction::~CPrediction( void )
{
}

void CPrediction::Init( void )
{
	m_bOldCLPredictValue = cl_predict.GetBool();
}

void CPrediction::Shutdown( void )
{
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrediction::CheckError( int commands_acknowledged )
{
	C_BasePlayer	*player;
	Vector		origin;
	Vector		delta;
	float		len;
	static int	pos = 0;

	// Not in the game yet
	if ( !engine->IsInGame() )
		return;

	// Not running prediction
	if ( !cl_predict.GetInt() )
		return;

	player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
		return;
	
	// Not predictable yet (flush entity packet?)
	if ( !player->IsIntermediateDataAllocated() )
		return;

	origin = player->GetNetworkOrigin();
		
	const void *slot = player->GetPredictedFrame( commands_acknowledged - 1 );
	if ( !slot )
		return;

	// Find the origin field in the database
	typedescription_t *td = FindFieldByName( "m_vecNetworkOrigin", player->GetPredDescMap() );
	Assert( td );
	if ( !td )
		return;

	Vector predicted_origin;

	memcpy( (Vector *)&predicted_origin, (Vector *)( (byte *)slot + td->fieldOffset[ PC_DATA_PACKED ] ), sizeof( Vector ) );
	
	// Compare what the server returned with what we had predicted it to be
	VectorSubtract ( predicted_origin, origin, delta );

	len = VectorLength( delta );
	if (len > MAX_PREDICTION_ERROR )
	{	
		// A teleport or something, clear out error
		m_vecPredictionError.Init();
		len = 0;
	}
	else
	{
		if ( cl_showerror.GetInt() >= 1 && ( len > MIN_PREDICTION_EPSILON ) )
		{
			con_nprint_t np;
			np.fixed_width_font = true;
			np.color[0] = 1.0f;
			np.color[1] = 0.95f;
			np.color[2] = 0.7f;
			np.index = 20 + ( ++pos % 20 );
			np.time_to_live = 2.0f;

			engine->Con_NXPrintf( &np, "pred error %6.3f units (%6.3f %6.3f %6.3f)", len, delta.x, delta.y, delta.z );
		}

		VectorCopy( delta, m_vecPredictionError );
	}

	// Only apply smoothing in multiplayer
	if ( len > MIN_CORRECTION_DISTANCE && 
		( engine->GetMaxClients() != 1 ) )
	{
		m_flCorrectionTime = cl_smoothtime.GetFloat();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrediction::ShutdownPredictables( void )
{
	// Transfer intermediate data from other predictables
	int c = predictables->GetPredictableCount();
	int i;

	int shutdown_count = 0;
	int release_count = 0;

	for ( i = c - 1; i >= 0 ; i-- )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		// Shutdown predictables
		if ( ent->GetPredictable() )
		{
			ent->ShutdownPredictable();
			shutdown_count++;
		}
		// Otherwise, release client created entities
		else
		{
			ent->Release();
			release_count++;
		}
	}

	if ( ( release_count > 0 ) || 
		 ( shutdown_count > 0 ) )
	{
		Msg( "Shutdown %i predictable entities and %i client-created entities\n",
			shutdown_count,
			release_count );
	}

	// All gone now...
	Assert( predictables->GetPredictableCount() == 0 );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrediction::ReinitPredictables( void )
{
	// Go through all entities and init any eligible ones
	int i;
	int c = ClientEntityList().GetHighestEntityIndex();
	for ( i = 0; i <= c; i++ )
	{
		C_BaseEntity *e = ClientEntityList().GetBaseEntity( i );
		if ( !e )
			continue;
		
		if ( e->GetPredictable() )
			continue;

		e->CheckInitPredictable( "ReinitPredictables" );
	}

	Msg( "Reinitialized %i predictable entities\n",
		predictables->GetPredictableCount() );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrediction::OnReceivedUncompressedPacket( void )
{
	m_nCommandsPredicted = 0;
	m_nServerCommandsAcknowledged = 0;
	m_nPreviousStartFrame = -1;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : commands_acknowledged - 
//			current_world_update_packet - 
// Output : void CPrediction::PreEntityPacketReceived
//-----------------------------------------------------------------------------
void CPrediction::PreEntityPacketReceived ( int commands_acknowledged, int current_world_update_packet )
{
	VPROF( "CPrediction::PreEntityPacketReceived" );

	// Cache off incoming packet #
	m_nIncomingPacketNumber = current_world_update_packet;

	// Don't screw up memory of current player from history buffers if not filling in history buffers
	//  during prediction!!!
	if ( !cl_predict.GetBool() )
	{
		ShutdownPredictables();
		return;
	}

	C_BasePlayer *current = C_BasePlayer::GetLocalPlayer();
	// No local player object?
	if ( !current )
		return;

	// Transfer intermediate data from other predictables
	int c = predictables->GetPredictableCount();
	int i;
	for ( i = 0; i < c; i++ )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		if ( !ent->GetPredictable() )
			continue;

		ent->PreEntityPacketReceived( commands_acknowledged );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called for every packet received( could be multiple times per frame)
//-----------------------------------------------------------------------------
void CPrediction::PostEntityPacketReceived( void )
{
	VPROF( "CPrediction::PostEntityPacketReceived" );

	// Don't screw up memory of current player from history buffers if not filling in history buffers
	//  during prediction!!!
	if ( !cl_predict.GetBool() )
		return;

	C_BasePlayer *current = C_BasePlayer::GetLocalPlayer();
	// No local player object?
	if ( !current )
		return;

	// Transfer intermediate data from other predictables
	int c = predictables->GetPredictableCount();
	int i;
	for ( i = 0; i < c; i++ )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		if ( !ent->GetPredictable() )
			continue;

		ent->PostEntityPacketReceived();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *ent - 
// Output : static bool
//-----------------------------------------------------------------------------
bool CPrediction::ShouldDumpEntity( C_BaseEntity *ent )
{
	int dump_entity = cl_predictionentitydump.GetInt();
	if ( dump_entity != -1 )
	{
		bool dump = false;
		if ( ent->entindex() == -1 )
		{
			dump = ( dump_entity == ent->entindex() ) ? true : false;
		}
		else
		{
			dump = ( ent->entindex() == dump_entity ) ? true : false;
		}

		if ( !dump )
		{
			return false;
		}
	}
	else
	{
		if ( cl_predictionentitydumpbyclass.GetString()[ 0 ] == 0 )
			return false;

		if ( !FClassnameIs( ent, cl_predictionentitydumpbyclass.GetString() ) )
			return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Called at the end of the frame if any packets were received
// Input  : error_check - 
//			last_predicted - 
//-----------------------------------------------------------------------------
void CPrediction::PostNetworkDataReceived( int commands_acknowledged )
{
	VPROF( "CPrediction::PostNetworkDataReceived" );

	bool error_check = ( commands_acknowledged > 0 ) ? true : false;

	CPDumpPanel *dump = GetPDumpPanel();

	//Msg( "%i/%i ack %i commands/slot\n",
	//	gpGlobals->framecount,
	//	gpGlobals->tickcount,
	//	commands_acknowledged - 1 );

//	m_nServerCommandsAcknowledged = commands_acknowledged;
	m_nServerCommandsAcknowledged += commands_acknowledged;
	m_bPreviousAckHadErrors = false;

	bool entityDumped = false;

	C_BasePlayer *current = C_BasePlayer::GetLocalPlayer();
	// No local player object?
	if ( !current )
		return;

	// Don't screw up memory of current player from history buffers if not filling in history buffers
	//  during prediction!!!
	if ( cl_predict.GetBool() )
	{
		int showlist = cl_predictionlist.GetInt();
		int totalsize = 0;
		int totalsize_intermediate = 0;

		con_nprint_t np;
		np.fixed_width_font = true;
		np.color[0] = 0.8f;
		np.color[1] = 1.0f;
		np.color[2] = 1.0f;
		np.time_to_live = 2.0f;

		// Transfer intermediate data from other predictables
		int c = predictables->GetPredictableCount();
		int i;
		for ( i = 0; i < c; i++ )
		{
			C_BaseEntity *ent = predictables->GetPredictable( i );
			if ( !ent )
				continue;

			if ( ent->GetPredictable() )
			{
			//	if ( ent->PostNetworkDataReceived( commands_acknowledged ) )
				if ( ent->PostNetworkDataReceived( m_nServerCommandsAcknowledged ) )
				{
					m_bPreviousAckHadErrors = true;
				}
			}

			if ( showlist )
			{
				char sz[ 32 ];
				if ( ent->entindex() == -1 )
				{
					Q_snprintf( sz, sizeof( sz ), "handle %u", (unsigned int)ent->GetClientHandle().ToInt() );
				}
				else
				{
					Q_snprintf( sz, sizeof( sz ), "%i", ent->entindex() );
				}

				np.index = i;

				if ( showlist >= 2 )
				{
					int size = GetClassMap().GetClassSize( ent->GetClassname() );
					int intermediate_size = ent->GetIntermediateDataSize() * ( MULTIPLAYER_BACKUP + 1 );

					engine->Con_NXPrintf( &np, "%15s %30s (%5i / %5i bytes): %15s", 
						sz, 
						ent->GetClassname(),
						size,
						intermediate_size,
						ent->GetPredictable() ? "predicted" : "client created" );

					totalsize += size;
					totalsize_intermediate += intermediate_size;
				}
				else
				{
					engine->Con_NXPrintf( &np, "%15s %30s: %15s", 
						sz, 
						ent->GetClassname(),
						ent->GetPredictable() ? "predicted" : "client created" );
				}
			}

			if ( error_check && 
				!entityDumped &&
				dump &&
				ShouldDumpEntity( ent ) )
			{
				entityDumped = true;
			//	dump->DumpEntity( ent, commands_acknowledged );
				dump->DumpEntity( ent, m_nServerCommandsAcknowledged );
			}
		}

		if ( showlist >= 2 )
		{
			np.index = i++;
			char sz1[32];
			char sz2[32];

			Q_strncpy( sz1, Q_pretifymem( (float)totalsize ), sizeof( sz1 ) );
			Q_strncpy( sz2, Q_pretifymem( (float)totalsize_intermediate ), sizeof( sz2 ) );

			engine->Con_NXPrintf( &np, "%15s %27s (%s / %s)  %14s", 
				"totals:", 
				"",
				sz1,
				sz2,
				"" );
		}

		// Zero out rest of list
		if ( showlist )
		{
			while ( i < 20 )
			{
				engine->Con_NPrintf( i, "" );
				i++;
			}
		}

		if ( error_check && cl_showerror.GetBool() )
		{
		//	CheckError( commands_acknowledged );
			CheckError( m_nServerCommandsAcknowledged );
		}
	}

	// Can also look at regular entities
	int dumpentindex = cl_predictionentitydump.GetInt();
	if ( dump && error_check && !entityDumped && dumpentindex != -1 )
	{
		int last_entity = ClientEntityList().GetHighestEntityIndex();
		if ( dumpentindex >= 0 && dumpentindex <= last_entity )
		{
			C_BaseEntity *ent = ClientEntityList().GetBaseEntity( dumpentindex );
			if ( ent )
			{
			//	dump->DumpEntity( ent, commands_acknowledged );
				dump->DumpEntity( ent, m_nServerCommandsAcknowledged );
				entityDumped = true;
			}
		}
	}

	if ( cl_predict.GetBool() != m_bOldCLPredictValue )
	{
		if ( !m_bOldCLPredictValue )
		{
			ReinitPredictables();
		}

		m_nCommandsPredicted = 0;
		m_nServerCommandsAcknowledged = 0;
		m_nPreviousStartFrame = -1;
	}

	m_bOldCLPredictValue = cl_predict.GetBool();

	m_vecCurrentNetworkOrigin = current->GetNetworkOrigin();

	if ( dump && error_check && !entityDumped )
	{
		dump->Clear();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Prepare for running prediction code
// Input  : *ucmd - 
//			*from - 
//			*pHelper - 
//			&moveInput - 
//-----------------------------------------------------------------------------
void CPrediction::SetupMove( C_BasePlayer *player, CUserCmd *ucmd, IMoveHelper *pHelper, CMoveData *move ) 
{
	VPROF( "CPrediction::SetupMove" );

	move->m_bFirstRunOfFunctions = IsFirstTimePredicted();
	
	move->m_nPlayerHandle = player->GetClientHandle();
	move->m_vecVelocity		= player->GetAbsVelocity();
	move->m_vecOrigin		= player->GetNetworkOrigin();
	move->m_vecOldAngles	= move->m_vecAngles;
	move->m_nOldButtons		= player->m_Local.m_nOldButtons;
	move->m_flClientMaxSpeed = player->m_flMaxspeed;

	move->m_vecAngles		= ucmd->viewangles;
	move->m_vecViewAngles	= ucmd->viewangles;
	move->m_nImpulseCommand = ucmd->impulse;	
	move->m_nButtons		= ucmd->buttons;

	CBaseEntity *pMoveParent = player->GetMoveParent();
	if (!pMoveParent)
	{
		move->m_vecAbsViewAngles = move->m_vecViewAngles;
	}
	else
	{
		matrix3x4_t viewToParent, viewToWorld;
		AngleMatrix( move->m_vecViewAngles, viewToParent );
		ConcatTransforms( pMoveParent->EntityToWorldTransform(), viewToParent, viewToWorld );
		MatrixAngles( viewToWorld, move->m_vecAbsViewAngles );
	}


	// Ingore buttons for movement if at controls
	if (player->GetFlags() & FL_ATCONTROLS)
	{
		move->m_flForwardMove		= 0;
		move->m_flSideMove			= 0;
		move->m_flUpMove			= 0;
	}
	else
	{
		move->m_flForwardMove		= ucmd->forwardmove;
		move->m_flSideMove			= ucmd->sidemove;
		move->m_flUpMove			= ucmd->upmove;
	}
		
	IClientVehicle *pVehicle = player->GetVehicle();
	if (pVehicle)
	{
		pVehicle->SetupMove( player, ucmd, pHelper, move ); 
	}

	// Copy constraint information
	if ( player->m_hConstraintEntity )
		move->m_vecConstraintCenter = player->m_hConstraintEntity->GetAbsOrigin();
	else
		move->m_vecConstraintCenter = player->m_vecConstraintCenter;

	move->m_flConstraintRadius = player->m_flConstraintRadius;
	move->m_flConstraintWidth = player->m_flConstraintWidth;
	move->m_flConstraintSpeedFactor = player->m_flConstraintSpeedFactor;
}

//-----------------------------------------------------------------------------
// Purpose: Finish running prediction code
// Input  : &move - 
//			*to - 
//-----------------------------------------------------------------------------
void CPrediction::FinishMove( C_BasePlayer *player, CUserCmd *ucmd, CMoveData *move )
{
	VPROF( "CPrediction::FinishMove" );

	player->m_RefEHandle = move->m_nPlayerHandle;

	player->m_vecVelocity = move->m_vecVelocity;

	player->m_vecNetworkOrigin = move->m_vecOrigin;
	
	player->m_Local.m_nOldButtons = move->m_nButtons;


	player->m_flMaxspeed = move->m_flClientMaxSpeed;
	
	m_hLastGround = player->m_hGroundEntity;
 
	player->SetLocalOrigin( move->m_vecOrigin );

	IClientVehicle *pVehicle = player->GetVehicle();
	if (pVehicle)
	{
		pVehicle->FinishMove( player, ucmd, move ); 
	}

	// Sanity checks
	if ( player->m_hConstraintEntity )
		Assert( move->m_vecConstraintCenter == player->m_hConstraintEntity->GetAbsOrigin() );
	else
		Assert( move->m_vecConstraintCenter == player->m_vecConstraintCenter );
	Assert( move->m_flConstraintRadius == player->m_flConstraintRadius );
	Assert( move->m_flConstraintWidth == player->m_flConstraintWidth );
	Assert( move->m_flConstraintSpeedFactor == player->m_flConstraintSpeedFactor );
}

//-----------------------------------------------------------------------------
// Purpose: Called before any movement processing
// Input  : *player - 
//			*cmd - 
//-----------------------------------------------------------------------------
void CPrediction::StartCommand( C_BasePlayer *player, CUserCmd *cmd )
{
	VPROF( "CPrediction::StartCommand" );

	CPredictableId::ResetInstanceCounters();

	player->m_pCurrentCommand = cmd;
	C_BaseEntity::SetPredictionRandomSeed( cmd );
	C_BaseEntity::SetPredictionPlayer( player );
}

//-----------------------------------------------------------------------------
// Purpose: Called after any movement processing
// Input  : *player - 
//-----------------------------------------------------------------------------
void CPrediction::FinishCommand( C_BasePlayer *player )
{
	VPROF( "CPrediction::FinishCommand" );

	player->m_pCurrentCommand = NULL;
	C_BaseEntity::SetPredictionRandomSeed( NULL );
	C_BaseEntity::SetPredictionPlayer( NULL );
}

//-----------------------------------------------------------------------------
// Purpose: Called before player thinks
// Input  : *player - 
//			thinktime - 
//-----------------------------------------------------------------------------
void CPrediction::RunPreThink( C_BasePlayer *player )
{
	VPROF( "CPrediction::RunPreThink" );

	// Run think functions on the player
	if ( !player->PhysicsRunThink() )
		return;

	// Called every frame to let game rules do any specific think logic for the player
	// FIXME:  Do we need to set up a client side version of the gamerules???
	// g_pGameRules->PlayerThink( player );

	player->PreThink();
}

//-----------------------------------------------------------------------------
// Purpose: Runs the PLAYER's thinking code if time.  There is some play in the exact time the think
//  function will be called, because it is called before any movement is done
//  in a frame.  Not used for pushmove objects, because they must be exact.
//  Returns false if the entity removed itself.
// Input  : *ent - 
//			frametime - 
//			clienttimebase - 
// Output : void CPlayerMove::RunThink
//-----------------------------------------------------------------------------
void CPrediction::RunThink (C_BasePlayer *player, double frametime )
{
	VPROF( "CPrediction::RunThink" );

	int thinktick = player->GetNextThinkTick();

	if ( thinktick <= 0 || thinktick > player->m_nTickBase )
		return;
	
	player->SetNextThink( TICK_NEVER_THINK );

	// Think
	player->Think();
}

//-----------------------------------------------------------------------------
// Purpose: Called after player movement
// Input  : *player - 
//			thinktime - 
//			frametime - 
//-----------------------------------------------------------------------------
void CPrediction::RunPostThink( C_BasePlayer *player )
{
	VPROF( "CPrediction::RunPostThink" );

	// Run post-think
	player->PostThink();
}

//-----------------------------------------------------------------------------
// Purpose: Predicts a single movement command for player
// Input  : *moveHelper - 
//			*player - 
//			*u - 
//-----------------------------------------------------------------------------
void CPrediction::RunCommand( C_BasePlayer *player, CUserCmd *ucmd, IMoveHelper *moveHelper )
{
	VPROF( "CPrediction::RunCommand" );

	StartCommand( player, ucmd );

	// Set globals appropriately
	gpGlobals->curtime		= player->m_nTickBase * TICK_RATE;
	gpGlobals->frametime	= ucmd->frametime;

// TODO
// TODO:  Check for impulse predicted?

	// Do weapon selection
	if ( ucmd->weaponselect != 0 )
	{
		C_BaseCombatWeapon *weapon = dynamic_cast< C_BaseCombatWeapon * >( CBaseEntity::Instance( ucmd->weaponselect ) );
		if ( weapon )
		{
			player->SelectItem( weapon->GetName(), ucmd->weaponsubtype );
		}
	}

//	// Assume the player isn't standing on any moving object
//	VectorClear ( player->m_Local.m_vecClientBaseVelocity );

	// Get button states
	player->UpdateButtonState( ucmd->buttons );

// TODO
//	CheckMovingGround( player, ucmd->frametime );

// TODO
//	g_pMoveData->m_vecOldAngles = player->pl.v_angle;

	// Copy from command to player unless game .dll has set angle using fixangle
	// if ( !player->pl.fixangle )
	{
		player->SetLocalViewAngles( ucmd->viewangles );
	}

	// Call standard client pre-think
	RunPreThink( player );

	// Call Think if one is set
	RunThink( player, ucmd->frametime );

	IClientVehicle *pVehicle = player->GetVehicle();

	// Setup input.
	{
	
		SetupMove( player, ucmd, moveHelper, g_pMoveData );
	}

	// RUN MOVEMENT
	if ( !pVehicle )
	{
		Assert( g_pGameMovement );
		g_pGameMovement->ProcessMovement( player, g_pMoveData );
	}
	else
	{
		pVehicle->ProcessMovement( player, g_pMoveData );
	}

	FinishMove( player, ucmd, g_pMoveData );

	RunPostThink( player );

// TODO:  Predict impacts?
//	// Let server invoke any needed impact functions
//	moveHelper->ProcessImpacts();

	FinishCommand( player );

	player->m_nTickBase++;
}

//-----------------------------------------------------------------------------
// Purpose: In the forward direction, creates rays straight down and determines the
//  height of the 'floor' hit for each forward test.  Then, if the samples show that the
//  player is about to enter an up/down slope, sets *idealpitch to look up or down that slope
//  as appropriate
//-----------------------------------------------------------------------------
void CPrediction::SetIdealPitch ( C_BasePlayer *player, const Vector& origin, const QAngle& angles, const Vector& viewheight )
{
	Vector	forward;
	Vector	top, bottom;
	float	floor_height[MAX_FORWARD];
	int		i, j;
	int		step, dir, steps;
	trace_t tr;

	if ( player->m_hGroundEntity == NULL )
		return;
		
	AngleVectors( angles, &forward );
	forward[2] = 0;

	// Now move forward by 36, 48, 60, etc. units from the eye position and drop lines straight down
	//  160 or so units to see what's below
	for (i=0 ; i<MAX_FORWARD ; i++)
	{
		VectorMA( origin, (i+3)*12, forward, top );
		
		top[2] += viewheight[ 2 ];

		VectorCopy( top, bottom );

		bottom[2] -= 160;

		UTIL_TraceLine( top, bottom, MASK_SOLID, NULL, COLLISION_GROUP_PLAYER_MOVEMENT, &tr );

		// looking at a wall, leave ideal the way it was
		if ( tr.allsolid )
			return;	

		// near a dropoff/ledge
		if ( tr.fraction == 1 )
			return;	
		
		floor_height[i] = top[2] + tr.fraction*( bottom[2] - top[2] );
	}
	
	dir = 0;
	steps = 0;
	for (j=1 ; j<i ; j++)
	{
		step = floor_height[j] - floor_height[j-1];
		if (step > -ON_EPSILON && step < ON_EPSILON)
			continue;

		if (dir && ( step-dir > ON_EPSILON || step-dir < -ON_EPSILON ) )
			return;		// mixed changes

		steps++;	
		dir = step;
	}
	
	if (!dir)
	{
		m_flIdealPitch = 0;
		return;
	}
	
	if (steps < 2)
		return;
	m_flIdealPitch = -dir * cl_idealpitchscale.GetFloat();
}

//-----------------------------------------------------------------------------
// Purpose: Walk backward through predictables looking for ClientCreated entities
//  such as projectiles which were
// 1) not actually ack'd by the server or
// 2) were ack'd and made dormant and can now safely be removed
// Input  : last_command_packet - 
//-----------------------------------------------------------------------------
void CPrediction::RemoveStalePredictedEntities( int command_mod )
{
	VPROF( "CPrediction::RemoveStalePredictedEntities" );

	CUserCmd *cmd = input->GetUsercmd( command_mod );
	Assert( cmd );

	int oldest_allowable_command = cmd->command_number;

	// Walk backward due to deletion from UtlVector
	int c = predictables->GetPredictableCount();
	int i;
	for ( i = c - 1; i >= 0; i-- )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		// Don't do anything to truly predicted things (like player and weapons )
		if ( ent->GetPredictable() )
			continue;

		// What's left should be things like projectiles that are just waiting to be "linked"
		//  to their server counterpart and deleted
		Assert( ent->IsClientCreated() );
		if ( !ent->IsClientCreated() )
			continue;

		// Snag the PredictionContext
		PredictionContext *ctx = ent->m_pPredictionContext;
		if ( !ctx )
		{
			continue;
		}

		// If it was ack'd then the server sent us the entity.
		// Leave it unless it wasn't made dormant this frame, in
		//  which case it can be removed now
		if ( ent->m_PredictableID.GetAcknowledged() )
		{
			// Hasn't become dormant yet!!!
			if ( !ent->IsDormantPredictable() )
			{
				Assert( 0 );
				continue;
			}

			// Still gets to live till next frame
			if ( ent->BecameDormantThisPacket() )
				continue;

			C_BaseEntity *serverEntity = ctx->m_hServerEntity;
			if ( serverEntity )
			{
				// Notify that it's going to go away
				serverEntity->OnPredictedEntityRemove( true, ent );
			}
		}
		else
		{
			// Check context to see if it's too old?
			int command_entity_creation_happened = ctx->m_nCreationCommandNumber;
			// Give it more time to live...not time to kill it yet
			if ( command_entity_creation_happened > oldest_allowable_command )
				continue;

			// If the client predicted the KILLME flag it's possible
			//  that entity had such a short life that it actually
			//  never was sent to us.  In that case, just let it die a silent death
			if ( !ent->IsEFlagSet( EFL_KILLME ) )
			{
				if ( cl_showerror.GetInt() != 0 )
				{
					// It's bogus, server doesn't have a match, destroy it:
					Msg( "Removing unack'ed predicted entity:  %s created %s(%i) id == %s : %p\n",
						ent->GetClassname(),
						ctx->m_pszCreationModule,
						ctx->m_nCreationLineNumber,
						ent->m_PredictableID.Describe(),
						ent );
				}
			}

			// FIXME:  Do we need an OnPredictedEntityRemove call with an "it's not valid"
			// flag of some kind
		}

		// This will remove it from predictables list and will also free the entity, etc.
		ent->Release();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrediction::RestoreOriginalEntityState( void )
{
	VPROF( "CPrediction::RestoreOriginalEntityState" );

	C_BaseEntity::EnableAbsRecomputations( true );

	// Transfer intermediate data from other predictables
	int pc = predictables->GetPredictableCount();
	int p;
	for ( p = 0; p < pc; p++ )
	{
		C_BaseEntity *ent = predictables->GetPredictable( p );
		if ( !ent )
			continue;

		if ( ent->GetPredictable() )
		{
			ent->RestoreData( "RestoreOriginalEntityState", C_BaseEntity::SLOT_ORIGINALDATA, PC_EVERYTHING );
		}

		// HACK Force recomputation of origin
		// VXP: Maybe, do this recursively: Source 2007: InvalidateEFlagsRecursive
		ent->m_iEFlags |= EFL_DIRTY_ABSTRANSFORM |
			EFL_DIRTY_ABSVELOCITY | EFL_DIRTY_ABSANGVELOCITY;

		// FIXME: This is a hack, we need to figure out where this should go...
		ent->MoveAimEnt();
		ent->Relink();
	}

	C_BaseEntity::EnableAbsRecomputations( false );

}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : current_command - 
//			curtime - 
//			*cmd - 
//			*tcmd - 
//			*localPlayer - 
//-----------------------------------------------------------------------------
void CPrediction::RunSimulation( int current_command, float curtime, CUserCmd *cmd, C_BasePlayer *localPlayer )
{
	VPROF( "CPrediction::RunSimulation" );

	Assert( localPlayer );
	C_CommandContext *ctx = localPlayer->GetCommandContext();
	Assert( ctx );
	
	ctx->needsprocessing = true;
	ctx->cmd = *cmd;
	ctx->command_number = current_command;

	IPredictionSystem::SuppressEvents( !IsFirstTimePredicted() );

	int i;

	// Make sure simulation occurs at most once per entity per usercmd
	for ( i = 0; i < predictables->GetPredictableCount(); i++ )
	{
		C_BaseEntity *entity = predictables->GetPredictable( i );
		if ( entity )
		{
			entity->m_nSimulationTick = -1;
		}
	}

	// Don't used cached numpredictables since entities can be created mid-prediction by the player
	for ( i = 0; i < predictables->GetPredictableCount(); i++ )
	{
		// Always reset
		gpGlobals->curtime		= curtime;
		gpGlobals->frametime	= cmd->frametime;

		C_BaseEntity *entity = predictables->GetPredictable( i );

		if ( !entity )
			continue;

		bool islocal = ( localPlayer == entity ) ? true : false;

		// Local player simulates first, if this assert fires then the predictables list isn't sorted 
		//  correctly (or we started predicting C_World???)
		if ( islocal )
		{
			Assert( i == 0 );
		}

		// Player can't be this so cull other entities here
		if ( entity->GetFlags() & FL_STATICPROP )
		{
			continue;
		}

		// Player is not actually in the m_SimulatedByThisPlayer list, of course
		if ( entity->IsPlayerSimulated() )
		{
			continue;
		}

		if ( AddDataChangeEvent( entity, DATA_UPDATE_DATATABLE_CHANGED, &entity->m_DataChangeEventRef ) )
		{
			entity->OnPreDataChanged( DATA_UPDATE_DATATABLE_CHANGED );
		}

		// Certain entities can be created locally and if so created, should be 
		//  simulated until a network update arrives
		if ( entity->IsClientCreated() )
		{
			// Only simulate these on new usercmds
			if ( !IsFirstTimePredicted() )
				continue;

			entity->PhysicsSimulate();
		}
		else
		{
			entity->PhysicsSimulate();
		}

		entity->OnLatchInterpolatedVariables( LATCH_SIMULATION_VAR | LATCH_ANIMATION_VAR );
	}

	// Always reset after running command
	IPredictionSystem::SuppressEvents( false );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrediction::Untouch( void )
{
	int numpredictables = predictables->GetPredictableCount();

	// Loop through all entities again, checking their untouch if flagged to do so
	int i;
	for ( i = 0; i < numpredictables; i++ )
	{
		C_BaseEntity *entity = predictables->GetPredictable( i );
		if ( !entity )
			continue;

		if ( !entity->GetCheckUntouch() )
			continue;

		entity->PhysicsCheckForEntityUntouch();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void InvalidateEFlagsRecursive( C_BaseEntity *pEnt, int nDirtyFlags, int nChildFlags = 0 )
{
	pEnt->AddEFlags( nDirtyFlags );
	nDirtyFlags |= nChildFlags;
	for (CBaseEntity *pChild = pEnt->FirstMoveChild(); pChild; pChild = pChild->NextMovePeer())
	{
		InvalidateEFlagsRecursive( pChild, nDirtyFlags );
	}
}

void CPrediction::StorePredictionResults( int predicted_frame )
{
	VPROF( "CPrediction::StorePredictionResults" );

	int i;
	int numpredictables = predictables->GetPredictableCount();

	// Now save off all of the results
	for ( i = 0; i < numpredictables; i++ )
	{
		C_BaseEntity *entity = predictables->GetPredictable( i );
		if ( !entity )
			continue;

		// HACK Force recomputation of origin
		// VXP: Maybe, do this recursively: Source 2007: InvalidateEFlagsRecursive
		entity->m_iEFlags |= EFL_DIRTY_ABSTRANSFORM | EFL_DIRTY_ABSVELOCITY | EFL_DIRTY_ABSANGVELOCITY;
		// FIXME: This is a hack, we need to figure out where this should go...
		entity->MoveAimEnt();
		entity->Relink();

		// Certain entities can be created locally and if so created, should be 
		//  simulated until a network update arrives
		if ( !entity->GetPredictable() )
			continue;

		entity->SaveData( "StorePredictionResults", predicted_frame, PC_EVERYTHING );
	}
}



//-----------------------------------------------------------------------------
// Purpose: 
// Input  : slots_to_remove - 
//			previous_last_slot - 
//-----------------------------------------------------------------------------
void CPrediction::ShiftIntermediateDataForward( int slots_to_remove, int number_of_commands_run )
{
	VPROF( "CPrediction::ShiftIntermediateDataForward" );

	C_BasePlayer *current = C_BasePlayer::GetLocalPlayer();
	// No local player object?
	if ( !current )
		return;

	// Don't screw up memory of current player from history buffers if not filling in history buffers
	//  during prediction!!!
	if ( !cl_predict.GetBool() )
		return;

	int c = predictables->GetPredictableCount();
	int i;
	for ( i = 0; i < c; i++ )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		if ( !ent->GetPredictable() )
			continue;

		ent->ShiftIntermediateDataForward( slots_to_remove, number_of_commands_run );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : predicted_frame - 
//-----------------------------------------------------------------------------
void CPrediction::RestoreEntityToPredictedFrame( int predicted_frame )
{
	VPROF( "CPrediction::RestoreEntityToPredictedFrame" );

	C_BasePlayer *current = C_BasePlayer::GetLocalPlayer();
	// No local player object?
	if ( !current )
		return;

	// Don't screw up memory of current player from history buffers if not filling in history buffers
	//  during prediction!!!
	if ( !cl_predict.GetBool() )
		return;

	int c = predictables->GetPredictableCount();
	int i;
	for ( i = 0; i < c; i++ )
	{
		C_BaseEntity *ent = predictables->GetPredictable( i );
		if ( !ent )
			continue;

		if ( !ent->GetPredictable() )
			continue;

		ent->RestoreData( "RestoreEntityToPredictedFrame", predicted_frame, PC_EVERYTHING );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Computes starting destination for intermediate prediction data results and
//  does any fixups required by network optimization
// Input  : received_new_world_update - 
//			incoming_acknowledged - 
// Output : int
//-----------------------------------------------------------------------------
int CPrediction::ComputeFirstCommandToExecute( bool received_new_world_update, int incoming_acknowledged, int outgoing_command )
{
	int destination_slot = 1;
	int skipahead = 0;

	// If we didn't receive a new update, just jump right up to the very 
	//  last command we created for this very frame since we couldn't have 
	//  had any errors without being notified by the server of such a case
	// NOTE:  received_new_world_update only gets set to false if cl_pred_optimize >= 1
	if ( !received_new_world_update )
	{
		// this is where we would normally start
		int start = incoming_acknowledged + 1;
		// outgoing_command is where we really want to start
		skipahead = max( 0, ( outgoing_command - start ) );
		// Don't start past the last predicted command, though, or we'll get prediction errors
		skipahead = min( skipahead, m_nCommandsPredicted  );

		if ( m_nCommandsPredicted != skipahead )
		{
			RestoreEntityToPredictedFrame( skipahead - 1 );
		}

		//Msg( "%i/%i no world, skip to %i restore from slot %i\n", 
		//	gpGlobals->framecount,
		//	gpGlobals->tickcount,
		//	skipahead,
		//	skipahead - 1 );
	}
	else
	{
		// Otherwise, there is a second optimization, wherein if we did receive an update, but no
		//  values differed (or were outside their epsilon) and the server actually acknowledged running
		//  one or more commands, then we can revert the entity to the predicted state from last frame, 
		//  shift the # of commands worth of intermediate state off of front the intermediate state array, and
		//  only predict the usercmd from the latest render frame.
		if ( cl_pred_optimize.GetInt() >= 2 && 
			!m_bPreviousAckHadErrors && 
			m_nCommandsPredicted > 0 && 
			m_nServerCommandsAcknowledged > 0 &&
			m_nServerCommandsAcknowledged <= m_nCommandsPredicted )
		{
			// Copy all of the previously predicted data back into entity so we can skip repredicting it
			// This is the final slot that we previously predicted
			RestoreEntityToPredictedFrame( m_nCommandsPredicted - 1 );

			// Shift intermediate state blocks down by # of commands ack'd
			ShiftIntermediateDataForward( m_nServerCommandsAcknowledged, m_nCommandsPredicted );
			
			// Only predict new commands (note, this should be the same number that we could compute
			//  above based on outgoing_command - incoming_acknowledged - 1
			skipahead = ( m_nCommandsPredicted - m_nServerCommandsAcknowledged );

			//Msg( "%i/%i optimize2, skip to %i restore from slot %i\n", 
			//	gpGlobals->framecount,
			//	gpGlobals->tickcount,
			//	skipahead,
			//	m_nCommandsPredicted - 1 );
		}
	}

	destination_slot += skipahead;

	// Always reset these values now that we've handled them
	m_nCommandsPredicted			= 0;
	m_bPreviousAckHadErrors			= false;
	m_nServerCommandsAcknowledged	= 0;

	return destination_slot;
}

//-----------------------------------------------------------------------------
// Actually does the prediction work, returns false if an error occurred
//-----------------------------------------------------------------------------
bool CPrediction::PerformPrediction( bool received_new_world_update, C_BasePlayer *localPlayer, int startframe, 
									int incoming_acknowledged, int outgoing_command )
{
	VPROF( "CPrediction::PerformPrediction" );

	cmd_t* tcmd = NULL;

	int CL_UPDATE_BACKUP	= engine->GetMaxClients() == 1 ? SINGLEPLAYER_BACKUP : MULTIPLAYER_BACKUP;
	int CL_UPDATE_MASK		= CL_UPDATE_BACKUP - 1;

	// FIXME: Remove this hack when we can!!
	// This allows us to sample the world when it may not be ready to be sampled
	partition->SuppressLists( PARTITION_ALL_CLIENT_EDICTS, false );
	C_BaseEntity::SetAbsQueriesValid( true );
	C_BaseEntity::EnableAbsRecomputations( true );

	m_bInPrediction = true;

	// Start at command after last one server has processed and 
	//  go until we get to targettime or we run out of new commands
	int i = ComputeFirstCommandToExecute( received_new_world_update, incoming_acknowledged, outgoing_command );

	//Msg( "%i/%i tickbase %i\n",
	//	gpGlobals->framecount,
	//	gpGlobals->tickcount,
	//	localPlayer->m_nTickBase );

	//for ( int k = 1; k < i; k++ )
	//{
	//	Msg( "%i/%i Skip final tick %i into slot %i\n", 
	//		gpGlobals->framecount, gpGlobals->tickcount,
	//		localPlayer->m_nTickBase - i + k + 1,
	//		k - 1 );
	//}

	Assert( i >= 1 );
	while ( 1 )
	{
		// We've run too far forward
		if ( i >= CL_UPDATE_BACKUP - 1 )
		{
			break;
		}

		// Incoming_acknowledged is the last usercmd the server acknowledged having acted upon
		int current_command		= incoming_acknowledged + i;
		int current_command_mod = current_command & CL_UPDATE_MASK;

		// We've caught up to the current command.
		if ( current_command > outgoing_command )
		{
			break;
		}

		// The command we are going to run is the next one after the last one the server ack'd ( i starts at 1 )
		tcmd = engine->GetCommand( current_command_mod );
		Assert( tcmd );

		CUserCmd *cmd = input->GetUsercmd( current_command_mod );
		Assert( cmd );

		// Is this the first time predicting this
		m_bFirstTimePredicted = !tcmd->processedfuncs;

		// Set globals appropriately
		float curtime		= ( localPlayer->m_nTickBase ) * TICK_RATE;

		RunSimulation( current_command, curtime, cmd, localPlayer );

		gpGlobals->curtime		= curtime;
		gpGlobals->frametime	= cmd->frametime;

		// Call untouch on any entities no longer predicted to be touching
		Untouch();

		// Store intermediate data into appropriate slot
		StorePredictionResults( i - 1 ); // Note that I starts at 1

		m_nCommandsPredicted = i;

		if ( current_command == outgoing_command )
		{
			localPlayer->m_nFinalPredictedTick = localPlayer->m_nTickBase;
		}
		/*
		if ( 0 )
		{
			localPlayer->m_nFinalPredictedTick = localPlayer->m_nTickBase;
			Msg( "%i/%i Latch final tick %i start == %i into slot %i\n", 
				gpGlobals->framecount, gpGlobals->tickcount,
				localPlayer->m_nFinalPredictedTick,
				localPlayer->m_nFinalPredictedTick - i,
				i - 1 );
		}
		*/

		/*
		Msg( "%i/%i Predicted command %i tickbase == %i first %s\n", 
			gpGlobals->framecount, gpGlobals->tickcount,
			m_nCommandsPredicted,
			localPlayer->m_nTickBase,
			m_bFirstTimePredicted ? "yes" : "no" );
		*/

		// Mark that we issued any needed sounds, of not done already
		tcmd->processedfuncs = true;

		// Copy the state over.
		i++;
	}

//	Msg( "%i : predicted %i commands forward, %i ack'd last frame, had errors %s\n", 
//		gpGlobals->tickcount, 
//		m_nCommandsPredicted, 
//		m_nServerCommandsAcknowledged,
//		m_bPreviousAckHadErrors ? "true" : "false" );


	m_bInPrediction = false;

	// FIXME: Remove
	partition->SuppressLists( PARTITION_ALL_CLIENT_EDICTS, true );
	C_BaseEntity::EnableAbsRecomputations( false );
	C_BaseEntity::SetAbsQueriesValid( false );

	// Somehow we looped past the end of the list (severe lag), don't predict at all
	if ( i >= CL_UPDATE_MASK )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Little wrapped
//-----------------------------------------------------------------------------
inline bool CPrediction::ShouldSmoothPredictedView( int nPredictedMoveParent ) const
{
	return ( (m_flCorrectionTime > 0.0) && 
			cl_smooth.GetInt() && 
			cl_smoothtime.GetFloat() && 
			(nPredictedMoveParent == m_nLastPredictedMoveParent) );
}


//-----------------------------------------------------------------------------
// Smooths the predicted view
//-----------------------------------------------------------------------------
void CPrediction::SmoothPredictedView( C_BasePlayer *localPlayer )
{
	// A little trickery because I can't guarantee that the C_BaseEntity *s will be valid the next frame
	// and I need to check if the move parent changed; so check the raw int.
	EHANDLE hMoveParent;
	hMoveParent.Set( localPlayer->GetMoveParent() );
	int nPredictedMoveParent = hMoveParent.ToInt();
	if (ShouldSmoothPredictedView(nPredictedMoveParent))
	{
		float frac;
		Vector delta;

		// Only decay timer once per frame
		m_flCorrectionTime -= gpGlobals->frametime;
	
		// Make sure smoothtime is postive
		if ( cl_smoothtime.GetFloat() <= 0.0 )
		{
			cl_smoothtime.SetValue( "0.1" );
		}

		// Clamp from 0 to cl_smoothtime.GetFloat()
		m_flCorrectionTime = max( 0.0, m_flCorrectionTime );
		m_flCorrectionTime = min( m_flCorrectionTime, cl_smoothtime.GetFloat() );

		// Compute backward interpolation fraction along full correction
		frac = 1.0 - m_flCorrectionTime / cl_smoothtime.GetFloat();

		// Determine how much error we still have to make up for
		VectorSubtract( localPlayer->GetLocalOrigin(), m_vecLastPredictedOrigin, delta );

		// Scale the error by the backlerp fraction
		VectorScale( delta, frac, delta );

		// Go some fraction of the way
		// FIXME, Probably can't do this any more
		localPlayer->SetLocalOrigin( m_vecLastPredictedOrigin + delta );
	}

	m_vecLastPredictedOrigin = localPlayer->GetLocalOrigin();
	m_nLastPredictedMoveParent = nPredictedMoveParent;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : startframe - 
//			validframe - 
//			incoming_acknowledged - 
//			outgoing_command - 
//-----------------------------------------------------------------------------
void CPrediction::Update( int startframe, bool validframe, 
						 int incoming_acknowledged, int outgoing_command )
{
	VPROF_BUDGET( "CPrediction::Update", VPROF_BUDGETGROUP_PREDICTION );

	bool received_new_world_update = true;

	// Still starting at same frame, so make sure we don't do extra prediction ,etc.
	if ( ( m_nPreviousStartFrame == startframe ) && 
		cl_pred_optimize.GetBool() &&
		cl_predict.GetBool() )
	{
		received_new_world_update = false;
	}

	m_nPreviousStartFrame = startframe;

	// Save off current timer values, etc.
	CGlobalVarsBase saveVars(true);
	saveVars = *gpGlobals;
	m_flTrueClientTime = gpGlobals->curtime;

	_Update( received_new_world_update, startframe, validframe, incoming_acknowledged, outgoing_command );

	// Restore current timer values, etc.
	*gpGlobals = saveVars;
}

//-----------------------------------------------------------------------------
// Do the dirty deed of predicting the local player
//-----------------------------------------------------------------------------
void CPrediction::_Update( bool received_new_world_update, int startframe, bool validframe, 
						 int incoming_acknowledged, int outgoing_command )
{
	C_BasePlayer *localPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !localPlayer )
		return;

	// Always using current view angles no matter what
	// NOTE: ViewAngles are always interpreted as being *relative* to the player
	QAngle viewangles;
	engine->GetViewAngles( viewangles );
	localPlayer->SetLocalAngles( viewangles );

	if ( !validframe )
	{
		return;
	}

	// If we are not doing prediction, copy authoritative value into velocity and angle.
	if ( !cl_predict.GetInt() )
	{
		// When not predicting, we at least must make sure the player
		// view angles match the view angles...
		localPlayer->SetLocalViewAngles( viewangles );
		return;
	}

	//if ( gpGlobals->frametime == 0.0f )
		//return;

	int CL_UPDATE_BACKUP	= engine->GetMaxClients() == 1 ? SINGLEPLAYER_BACKUP : MULTIPLAYER_BACKUP;
	int CL_UPDATE_MASK		= CL_UPDATE_BACKUP - 1;

	// This is cheesy, but if we have entities that are parented to attachments on other entities, then 
	// it'll wind up needing to get a bone transform.
	C_BaseAnimating::InvalidateBoneCaches();
	C_BaseAnimating::AllowBoneAccess( true, true );

		// Remove any purely client predicted entities that were left "dangling" because the 
		//  server didn't acknowledge them or which can now safely be removed
		RemoveStalePredictedEntities( incoming_acknowledged & CL_UPDATE_MASK );

		// Restore objects back to "pristine" state from last network/world state update
		if ( received_new_world_update )
		{
			RestoreOriginalEntityState();
		}

		bool bPerform = PerformPrediction( received_new_world_update, localPlayer, startframe, incoming_acknowledged, outgoing_command );

	C_BaseAnimating::AllowBoneAccess( false, false );

	if ( !bPerform )
		return;

	// Overwrite predicted angles with the actual view angles
	localPlayer->SetLocalAngles( viewangles );

	// Smooth out view if prediction is erroring and we're not standing on a train or something
	SmoothPredictedView( localPlayer );

	// FIXME: Remove this hack when we can!!
	// This allows us to sample the world when it may not be ready to be sampled
	partition->SuppressLists( PARTITION_ALL_CLIENT_EDICTS, false );
	C_BaseEntity::SetAbsQueriesValid( true );

	// FIXME: What about hierarchy here?!?
	SetIdealPitch( localPlayer, localPlayer->GetLocalOrigin(), localPlayer->GetLocalAngles(), localPlayer->m_vecViewOffset );

	// FIXME: Remove this hack when we can!!
	partition->SuppressLists( PARTITION_ALL_CLIENT_EDICTS, true );
	C_BaseEntity::SetAbsQueriesValid( false );
}



//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CPrediction::InPrediction( void ) const
{
	return m_bInPrediction;
}
	
//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CPrediction::IsFirstTimePredicted( void ) const
{
	return m_bFirstTimePredicted;
}

// The engine needs to be able to access a few predicted values
int CPrediction::GetWaterLevel( void )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
		return 0;

	return player->m_nWaterLevel;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : org - 
//-----------------------------------------------------------------------------
void CPrediction::GetViewOrigin( Vector& org )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
	{
		org.Init();
	}
	else 
	{
		org = player->GetLocalOrigin();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : org - 
//-----------------------------------------------------------------------------
void CPrediction::SetViewOrigin( Vector& org )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
		return;

	player->SetLocalOrigin( org );
	player->m_vecNetworkOrigin = org;

	player->m_iv_vecOrigin.Reset();
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : ang - 
//-----------------------------------------------------------------------------
void CPrediction::GetViewAngles( QAngle& ang )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
	{
		ang.Init();
	}
	else 
	{
		ang = player->GetLocalAngles();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : ang - 
//-----------------------------------------------------------------------------
void CPrediction::SetViewAngles( QAngle& ang )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
		return;

	player->SetLocalAngles( ang );
	// player->SetLocalViewAngles( ang );
	player->m_angNetworkAngles = ang;

	player->m_iv_angRotation.Reset();
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : ang - 
//-----------------------------------------------------------------------------
void CPrediction::GetLocalViewAngles( QAngle& ang )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
	{
		ang.Init();
	}
	else 
	{
		ang = player->pl.v_angle;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : ang - 
//-----------------------------------------------------------------------------
void CPrediction::SetLocalViewAngles( QAngle& ang )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
		return;

	player->SetLocalViewAngles( ang );
}

//-----------------------------------------------------------------------------
// Purpose: For determining that predicted creation entities are un-acked and should
//  be deleted
// Output : int
//-----------------------------------------------------------------------------
int CPrediction::GetIncomingPacketNumber( void ) const
{
	return m_nIncomingPacketNumber;
}

//-----------------------------------------------------------------------------
// Purpose: The player is interpolating his eyes nice and smoothly forward during
//  demo playback.  If we're standing on something that's moving along with us, assume
//  that it's tied to our eye position as it interpolates, maintaining our original offset from it as it
//  moves.
// Input  : eyePosition - 
//-----------------------------------------------------------------------------
void CPrediction::DemoReportInterpolatedPlayerOrigin( const Vector& eyePosition )
{
	C_BasePlayer *player = C_BasePlayer::GetLocalPlayer();
	if ( !player )
		return;
	
	// Don't do this if hierarchically attached
	if ( player->GetMoveParent() != NULL )
	{
		return;
	}

	C_BaseEntity *ground = player->GetGroundEntity();
	if ( !ground )
		return;

	// The world never moves
	if ( ground->index == 0 )
		return;

	Vector org2 = ground->m_iv_vecOrigin.GetPrev();
	Vector org1 = ground->GetLocalOrigin();

	if ( org1 == org2 )
		return;
	
	Vector groundMoveDir = org1 - org2;
	VectorNormalize( groundMoveDir );

	Vector offset = eyePosition - m_vecCurrentNetworkOrigin;
	Vector playerMoveDir = offset;
	VectorNormalize( playerMoveDir );

	float dot = playerMoveDir.Dot( groundMoveDir );
	// Make sure it's not hozed, and it's along the same direction (i.e., it's the thing we're standing on that's moving!)
	if ( dot >= 0.99 && offset.Length() < 100.0f )
	{
		Vector origin;
		// Put the entity we're standing on at the same offset from it's last network position as our eyes have moved
		//  from our last network position.

		// Note that we also detect this case in C_BaseEntity::Interpolate
		origin = ground->GetNetworkOrigin() + offset;
		ground->SetLocalOrigin( origin );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : float
//-----------------------------------------------------------------------------
float CPrediction::GetTrueClientTime() const
{
	if ( InPrediction() )
	{
		return m_flTrueClientTime;
	}
	return gpGlobals->curtime;
}
