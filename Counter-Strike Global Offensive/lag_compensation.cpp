#include "aimbot.hpp"
#include "source.hpp"
#include "entity.hpp"
#include "player.hpp"
#include "weapon.hpp"
#include "hooked.hpp"
#include "math.hpp"
#include "displacement.hpp"
#include "lag_compensation.hpp"
#include "autowall.hpp"
#include "angle_resolver.hpp"
#include "game_movement.h"
#include "anti_aimbot.hpp"
#include <unordered_map>
#include <algorithm>
#include "rmenu.hpp"

#define LAG_COMPENSATION_TICKS 18
#define EFL_DIRTY_ABSVELOCITY (1<<12)

void c_lagcomp::get_interpolation( ) {
	static auto cl_interp = Source::m_pCvar->FindVar( "cl_interp" );
	static auto cl_interp_ratio = Source::m_pCvar->FindVar( "cl_interp_ratio" );
	static auto sv_client_min_interp_ratio = Source::m_pCvar->FindVar( "sv_client_min_interp_ratio" );
	static auto sv_client_max_interp_ratio = Source::m_pCvar->FindVar( "sv_client_max_interp_ratio" );
	static auto cl_updaterate = Source::m_pCvar->FindVar( "cl_updaterate" );
	static auto sv_minupdaterate = Source::m_pCvar->FindVar( "sv_minupdaterate" );
	static auto sv_maxupdaterate = Source::m_pCvar->FindVar( "sv_maxupdaterate" );

	auto updaterate = std::clamp( cl_updaterate->GetFloat( ), sv_minupdaterate->GetFloat( ), sv_maxupdaterate->GetFloat( ) );
	auto lerp_ratio = std::clamp( cl_interp_ratio->GetFloat( ), sv_client_min_interp_ratio->GetFloat( ), sv_client_max_interp_ratio->GetFloat( ) );
	cheat::main::lerp_time = std::clamp( lerp_ratio / updaterate, cl_interp->GetFloat( ), 1.0f );
}

void c_lagcomp::store_record_data( C_BasePlayer* entity, C_Tickrecord* record_data ) {
	if ( entity == nullptr || entity->get_animation_state( ) == nullptr )
		return;

	auto activity = entity->get_sec_activity( entity->get_animation_layer( 1 ).m_nSequence );
	auto priority = ( ( activity == ACT_CSGO_RELOAD ) && entity->get_animation_layer( 1 ).m_flWeight > 0.001f &&
					  entity->get_animation_layer( 1 ).m_flCycle < 1.f ) || entity->m_vecVelocity( ).Length2D( ) > 80.f;

	/* set the record to normal */
	record_data->type = RECORD_NORMAL; /* always */

	if ( cheat::features::aaa.player_resolver_records[ entity->entindex( ) - 1 ].did_lby_flick ) {
		record_data->shot_this_tick = true;
		record_data->type = RECORD_SHOT;
		record_data->shot_time = entity->m_flSimulationTime( );
	}
	else if ( priority ) {
		record_data->type = RECORD_PRIORITY;
	}

	memcpy( record_data->matrixes, entity->m_CachedBoneData( ).Base( ), entity->GetBoneCount( ) * sizeof( matrix3x4_t ) );

	if ( record_data->type == RECORD_NORMAL && records[ entity->entindex( ) - 1 ].m_Tickrecords.size( ) > 1 ) {
		record_data->type = ( ( fabs( entity->m_angEyeAngles( ).x ) < 45.f && fabs( records[ entity->entindex( ) - 1 ].m_Tickrecords.at( 1 ).eye_angles.x ) > 45.f ) ? RECORD_PRIORITY : RECORD_NORMAL );
	}

	record_data->abs_origin = entity->get_abs_origin( );
	record_data->object_mins = entity->OBBMins( );
	record_data->object_maxs = entity->OBBMaxs( );

	record_data->hitbox_positon = entity->get_bone_pos( 8 );
	record_data->origin = entity->m_vecOrigin( );
	record_data->velocity = entity->m_vecVelocity( );
	record_data->eye_angles = entity->m_angEyeAngles( );
	record_data->abs_eye_angles = entity->get_abs_eye_angles( );
	record_data->sequence = entity->m_nSequence( );
	record_data->entity_flags = entity->m_fFlags( );
	record_data->simulation_time = entity->m_flSimulationTime( );
	record_data->lower_body_yaw = entity->m_flLowerBodyYawTarget( );
	record_data->cycle = entity->m_flCycle( );
	record_data->dormant = entity->IsDormant( );
	record_data->anim_velocity = entity->m_vecVelocity( );
	record_data->ientity_flags = entity->m_iEFlags( );
	record_data->duck_amt = entity->m_flDuckAmount( );
	record_data->bones_count = entity->GetBoneCount( );
	record_data->lby_delta = 0.f;// cheat::features::aaa.sub_59B13C30(entity->get_animation_state());
	record_data->skin = entity->m_nSkin( );
	record_data->body = entity->m_nBody( );

	record_data->resolver_method = cheat::features::aaa.player_resolver_records[ entity->entindex( ) - 1 ].resolving_method;

	record_data->pose_paramaters = entity->m_flPoseParameter( );
	memcpy( record_data->anim_layers, entity->animation_layers_ptr( ), 0x38 * entity->get_animation_layers_count( ) );


	record_data->tick_count = Source::m_pGlobalVars->tickcount;
	record_data->data_filled = true;
}

void c_lagcomp::apply_record_data( C_BasePlayer* entity, C_Tickrecord* record_data, bool backup ) // might be better 
{
	if ( entity == nullptr || !record_data->data_filled || !entity->get_animation_state( ) )
		return;

	record_data->bones_count = Math::clamp( record_data->bones_count, 0, 128 );

	entity->GetBoneCount( ) = record_data->bones_count;
	entity->set_collision_bounds( record_data->object_mins, record_data->object_maxs );
	entity->set_abs_origin( backup ? record_data->abs_origin : record_data->origin );
	std::memcpy( entity->m_CachedBoneData( ).Base( ), record_data->matrixes, record_data->bones_count * sizeof( matrix3x4_t ) );
}

struct simulation_data {
public:
	simulation_data( C_BasePlayer* ent, C_Tickrecord* current );
	simulation_data( C_BasePlayer* ent );
public:
	C_BasePlayer* m_ent;
	Vector m_origin, m_velocity, m_maxs, m_mins;
	float m_move_angle;
	int m_flags;
};

simulation_data::simulation_data( C_BasePlayer* ent, C_Tickrecord* current ) {
	this->m_ent = ent;
	this->m_origin = current->origin;
	this->m_velocity = current->velocity;
	this->m_move_angle = RAD2DEG( std::atan2( current->velocity.y, current->velocity.x ) );
	this->m_mins = current->object_mins;
	this->m_maxs = current->object_maxs;
	this->m_flags = current->entity_flags;
}

simulation_data::simulation_data( C_BasePlayer* ent ) {
	this->m_ent = ent;
	this->m_origin = ent->get_abs_origin( );
	this->m_velocity = ent->m_vecVelocity( );
	this->m_move_angle = RAD2DEG( std::atan2( ent->m_vecVelocity( ).y, ent->m_vecVelocity( ).x ) );
	this->m_mins = ent->OBBMins( );
	this->m_maxs = ent->OBBMaxs( );
	this->m_flags = ent->m_fFlags( );
}

bool c_lagcomp::extrapolate( C_BasePlayer* ent, const int ticks, C_Tickrecord* extrapolated_record ) {
	if ( ticks == 0 )
		return false;

	if ( !extrapolated_record )
		return false;

	auto track = &records[ ent->entindex( ) - 1 ];

	if ( !track )
		return false;

	if ( track->m_Tickrecords.size( ) < 2 )
		return false;

	auto current_rec = &track->m_Tickrecords.front( );
	auto previous_rec = &track->m_Tickrecords.at( 1 );

	if ( !current_rec || !previous_rec )
		return false;

	simulation_data data( ent, current_rec );

	const auto cur_move_angle = data.m_move_angle;
	const auto prev_move_angle = RAD2DEG( std::atan2( previous_rec->velocity.y, previous_rec->velocity.x ) );
	auto move_angle_delta = ( cur_move_angle - prev_move_angle ) / ( current_rec->simulation_time - previous_rec->simulation_time );
	move_angle_delta = Math::normalize_angle( move_angle_delta );

	// note: you could improve accuracy slightly by accounting for acceleration,
	// I have the calcs done for my lag fix but it seems unnecessary for this.
	//const auto move_speed = data.m_velocity.Length2D();
	const auto move_speed = ( ( current_rec->origin - previous_rec->origin ) * ( 1.f / ( current_rec->simulation_time - previous_rec->simulation_time ) ) ).Length2D( );
	const auto should_jump = !( current_rec->entity_flags & FL_ONGROUND ) || !( previous_rec->entity_flags & FL_ONGROUND );

	for ( auto i = 0; i < ticks; i++ ) {
		// add the turn amount in a single tick to our current angle.
		data.m_move_angle += move_angle_delta * Source::m_pGlobalVars->interval_per_tick;
		data.m_move_angle = Math::normalize_angle( data.m_move_angle );

		// adjust our movement direction to reflect our current angle.
		data.m_velocity.x = std::cos( DEG2RAD( data.m_move_angle ) ) * move_speed;
		data.m_velocity.y = std::sin( DEG2RAD( data.m_move_angle ) ) * move_speed;

		if ( !( data.m_flags & FL_ONGROUND ) ) {
			// if we're in the air, subtract gravity.
			static auto sv_gravity = Source::m_pCvar->FindVar( "sv_gravity" );
			data.m_velocity.z -= ( sv_gravity->GetFloat( ) * Source::m_pGlobalVars->interval_per_tick );
		}
		else if ( should_jump ) {
			static auto sv_jump_impulse = Source::m_pCvar->FindVar( "sv_jump_impulse" );
			data.m_velocity.z = sv_jump_impulse->GetFloat( );
		}

		auto src = data.m_origin;
		auto end = src + ( data.m_velocity * Source::m_pGlobalVars->interval_per_tick );

		Ray_t ray;
		ray.Init( src, end, data.m_mins, data.m_maxs );

		trace_t trace;
		CTraceFilter filter;
		filter.pSkip = ent;
		//TraceLine(src, end, CONTENTS_SOLID, ent, &trace);
		Source::m_pEngineTrace->TraceRay( ray, CONTENTS_SOLID, &filter, &trace );
		// fix for grounded walking entities colliding with ground?
		const auto along_ground = ( data.m_flags & FL_ONGROUND ) && trace.fraction != 1.f && !should_jump && data.m_velocity.Dot( trace.plane.normal ) <= FLT_EPSILON;

		if ( along_ground ) {
			const auto mod = Vector( 0, 0, 1 );
			ray.Init( src + mod, end + mod, data.m_mins, data.m_maxs );
			Source::m_pEngineTrace->TraceRay( ray, CONTENTS_SOLID, &filter, &trace );
		}
		// if we hit a plane, reflect the vector along the plane in the nearest non-colliding direction.
		if ( trace.fraction != 1.f ) {
			for ( auto c = 0U; c < 2; c++ ) {
				data.m_velocity -= trace.plane.normal * data.m_velocity.Dot( trace.plane.normal );
				const auto dot = data.m_velocity.Dot( trace.plane.normal );
				if ( dot < 0.f )
					data.m_velocity -= ( trace.plane.normal * dot );
				end = trace.endpos + ( data.m_velocity * ( Source::m_pGlobalVars->interval_per_tick * ( 1.f - trace.fraction ) ) );
				ray.Init( trace.endpos, end, data.m_mins, data.m_maxs );
				Source::m_pEngineTrace->TraceRay( ray, CONTENTS_SOLID, &filter, &trace );
				if ( trace.fraction == 1.f )
					break;
			}
		}

		//Vector3 s[2];
		//if (Hack.Misc->WorldToScreen(data.m_origin, s[0]) && Hack.Misc->WorldToScreen(trace.endpos, s[1]))
		//	Hack.Drawings->DrawLine(s[0].x, s[0].y, s[1].x, s[1].y, Color::Red());
		data.m_origin = trace.endpos;
		// we have to check if we hit ground here, in case we need to simulate a jump next simulated tick.
		end = trace.endpos;
		//end.z -= 2.f;

		ray.Init( data.m_origin, end, data.m_mins, data.m_maxs );
		Source::m_pEngineTrace->TraceRay( ray, CONTENTS_SOLID, &filter, &trace );

		data.m_flags &= ~FL_ONGROUND;

		if ( trace.DidHit( ) && trace.plane.normal.z > 0.7f )
			data.m_flags |= FL_ONGROUND;

		current_rec->simulation_time += Source::m_pGlobalVars->interval_per_tick;
	}

	// copy the current record over so all the other data is recent.
	*extrapolated_record = *current_rec;

	// set the extrapolated information.
	extrapolated_record->origin = data.m_origin;
	extrapolated_record->velocity = data.m_velocity;
	extrapolated_record->entity_flags = data.m_flags;

	return true;
}

void c_lagcomp::accelerate_velocity( C_BasePlayer* player, Vector& velocity, Vector new_velocity_angle, Vector old_velocity_angle ) {
	static auto maxspeed = Source::m_pCvar->FindVar( "sv_maxspeed" );

	auto fl_maxspeed = maxspeed->GetFloat( );
	Vector forward, right, up;
	Math::AngleVectors( old_velocity_angle, &forward, &right, &up );

	forward[ 2 ] = 0.f;
	right[ 2 ] = 0.f;

	forward.Normalize( );
	right.Normalize( );

	auto wishvel = right * abs( old_velocity_angle.y - new_velocity_angle.y ); // note this only works because we assume cstrafe
	wishvel[ 2 ] = 0;

	auto wishdir = wishvel;
	auto wishspd = wishdir.Length( );

	if ( wishspd != 0.f )
		wishdir /= wishspd;

	if ( wishspd != 0.f && wishspd > fl_maxspeed )
		wishspd = fl_maxspeed;

	if ( wishspd > 30.f )
		wishspd = 30.f;

	const auto currentspeed = velocity.Dot( wishdir );
	const auto addspeed = wishspd - currentspeed;

	if ( addspeed <= 0 ) {
		const auto speed = velocity.Length2D( );
		velocity.x = std::cosf( DEG2RAD( new_velocity_angle.y ) ) * ( speed );
		velocity.y = std::sinf( DEG2RAD( new_velocity_angle.y ) ) * ( speed );
		return;
	}

	static ConVar* sv_airaccelerate = Source::m_pCvar->FindVar( "sv_accelerate" );

	auto accelspeed = sv_airaccelerate->GetFloat( ) * wishspd * Source::m_pGlobalVars->interval_per_tick;
	if ( accelspeed > addspeed )
		accelspeed = addspeed;

	const auto speed = velocity.Length2D( );
	velocity.x = std::cosf( DEG2RAD( new_velocity_angle.y ) ) * ( speed + accelspeed );
	velocity.y = std::sinf( DEG2RAD( new_velocity_angle.y ) ) * ( speed + accelspeed );
}

bool c_lagcomp::is_time_delta_too_large( C_Tickrecord* wish_record ) {
	const auto net = Source::m_pEngine->GetNetChannelInfo( );
	static auto sv_maxunlag = Source::m_pCvar->FindVar( "sv_maxunlag" );

	const auto outgoing = net->GetLatency( FLOW_OUTGOING );
	const auto incoming = net->GetLatency( FLOW_INCOMING );

	float correct = 0.f;
	correct += outgoing;
	correct += incoming;
	correct += cheat::main::lerp_time;
	correct = std::clamp( correct, 0.f, sv_maxunlag->GetFloat( ) );

	float deltaTime = std::abs( correct - ( Source::m_pGlobalVars->curtime - wish_record->simulation_time ) );

	return deltaTime > 0.2f;
}

bool c_lagcomp::is_time_delta_too_large( const float& simulation_time ) {
	float correct = 0.f;

	correct += Source::m_pEngine->GetNetChannelInfo( )->GetLatency( 0 );
	correct += Source::m_pEngine->GetNetChannelInfo( )->GetLatency( 1 );
	correct += cheat::main::lerp_time;

	static auto sv_maxunlag = Source::m_pCvar->FindVar( "sv_maxunlag" );
	correct = std::clamp( correct, 0.f, sv_maxunlag->GetFloat( ) );

	float deltaTime = std::abs( correct - ( Source::m_pGlobalVars->curtime - simulation_time ) );
	return deltaTime > 0.2f;
}

int c_lagcomp::start_lag_compensation( C_BasePlayer* entity, int wish_tick, C_Tickrecord* output_record ) {
	auto player_index = entity->entindex( ) - 1;
	auto player_record = &records[ player_index ];

	if ( player_record->m_Tickrecords.empty( ) )// || (wish_tick + 1 > player_record->m_Tickrecords.size() - 1))
		return -1;

	auto current_record = player_record->m_Tickrecords.at( wish_tick );
	//auto next_record = player_record->m_Tickrecords.at(wish_tick + 1);

	if ( !current_record.data_filled /*|| !next_record.data_filled*/ || wish_tick > 0 && is_time_delta_too_large( &current_record ) )
		return -1;

	if ( ( player_record->m_Tickrecords.size( ) > ( wish_tick + 1 ) ) && wish_tick == 0 && ( current_record.origin -
		 player_record->m_Tickrecords.at( wish_tick + 1 ).origin ).LengthSquared( ) > 4096.f && !current_record.origin.IsZero( ) &&
		 !player_record->m_Tickrecords.at( wish_tick + 1 ).origin.IsZero( ) && current_record.tick_count != Source::m_pGlobalVars->tickcount && entity->m_vecVelocity( ).Length2D( ) > 270.f ) {
		//predict_player(entity, &current_record, &player_record->m_Tickrecords.at(wish_tick + 1));
		apply_record_data( entity, &current_record );
		entity->force_bone_rebuild( );
		entity->SetupBonesEx( );
		store_record_data( entity, &current_record );
	}

	if ( output_record != nullptr && current_record.data_filled )
		*output_record = current_record;

	return TIME_TO_TICKS( current_record.simulation_time + cheat::main::lerp_time );

}

bool got_update[ 65 ];
float flick_time[ 65 ];

void c_lagcomp::update_player_record_data( C_BasePlayer* entity ) {
	if ( entity == nullptr )
		return;

	auto player_index = entity->entindex( ) - 1;
	auto player_record = &records[ player_index ];

	const float tickrate = ( 1 / Source::m_pGlobalVars->interval_per_tick ) / 4.f;
	const auto wtf = abs( Source::m_pGlobalVars->tickcount - TIME_TO_TICKS( entity->m_flSimulationTime( ) ) );
	auto unlag = wtf <= tickrate;

	if ( entity->m_flSimulationTime( ) > 0.f && player_record->m_Tickrecords.empty( ) && unlag
		 || !player_record->m_Tickrecords.empty( ) && player_record->m_Tickrecords.front( ).simulation_time
		 != entity->m_flSimulationTime( ) && unlag ) {
		update_animations_data( entity );

		if ( entity->m_flSimulationTime( ) > 0.f && player_record->m_Tickrecords.empty( ) || player_record->m_Tickrecords.front( ).simulation_time < entity->m_flSimulationTime( ) ) {
			C_Tickrecord new_record;
			store_record_data( entity, &new_record );

			if ( new_record.data_filled )
				player_record->m_Tickrecords.push_front( new_record );
		}

		while ( player_record->m_Tickrecords.size( ) > 64 )
			player_record->m_Tickrecords.pop_back( );
	}
	else
		entity->client_side_animation( ) = true;
}

void c_lagcomp::start_position_adjustment( ) {
	if ( !cheat::Cvars.RageBot_AdjustPositions.GetValue( ) )
		return;

	//cheat::main::fast_autostop = false;

	for ( auto index = 1; index < Source::m_pGlobalVars->maxClients; index++ ) {
		auto entity = ( C_BasePlayer* )Source::m_pEntList->GetClientEntity( index );

		if ( !entity || !entity->IsPlayer( ) )
			continue;

		if ( entity->IsDead( ) && entity->m_iTeamNum( ) != cheat::main::local( )->m_iTeamNum( ) ) {
			cheat::main::shots_fired[ index - 1 ] = 0;
			cheat::main::shots_total[ index - 1 ] = 0;
			memset( cheat::features::aaa.player_resolver_records[ index - 1 ].missed_shots, 0, sizeof( int ) * 11 );
		}

		if ( entity->IsDormant( ) ||
			 entity->m_iHealth( ) <= 0 ||
			 entity->IsDead( ) ||
			 entity->m_iTeamNum( ) == cheat::main::local( )->m_iTeamNum( )
			 ) {
			auto entidx = entity->entindex( );

			if ( entidx == 0 )
				continue;

			if ( records[ entidx - 1 ].tick_count != -1 )
				records[ entidx - 1 ].reset( ( entity->IsDormant( ) && entity->m_iHealth( ) <= 0 ) );

			got_update[ entidx - 1 ] = false;
			continue;
		}

		//auto player_record = &records[entity->entindex() - 1];
		//
		//if (player_record->m_Tickrecords.size() <= 2)
		//	continue;
		//
		//player_record->being_lag_compensated = true;
		//start_position_adjustment(entity);
		//player_record->being_lag_compensated = false;
	}
}

void c_lagcomp::start_position_adjustment( C_BasePlayer* entity ) {
	auto player_index = entity->entindex( ) - 1;
	auto player_record = &records[ player_index ];

	auto local_weapon = ( C_WeaponCSBaseGun* )( Source::m_pEntList->GetClientEntityFromHandle( cheat::main::local( )->m_hActiveWeapon( ) ) );

	if ( player_record->m_Tickrecords.empty( ) || !local_weapon )
		return;

	auto has_knife = ( local_weapon->m_iItemDefinitionIndex( ) == weapon_knife_ct || local_weapon->m_iItemDefinitionIndex( ) == weapon_knife_t );
	auto has_zeus = local_weapon->m_iItemDefinitionIndex( ) == weapon_taser;

	player_record->backtrack_ticks = 0;
	player_record->tick_count = -1;
	player_record->type = 0;
	player_record->hitbox_position.Set( );
	player_record->matrix_valid = false;

	float max_dmg = 1.f;
	TickrecordType type = RECORD_NORMAL;
	float max_distance = 8196.f;

	// Just normally loop as we would
	for ( int index = 0; index < player_record->m_Tickrecords.size( ); index++ ) {
		if ( ( index + 1 ) > ( player_record->m_Tickrecords.size( ) - 1 ) )
			break;

		auto current_record = &player_record->m_Tickrecords.at( index );

		C_Tickrecord* prev_record = nullptr;

		if ( player_record->m_Tickrecords.size( ) > 2 && index < ( player_record->m_Tickrecords.size( ) - 1 ) ) {
			if ( !is_time_delta_too_large( current_record ) )
				prev_record = &player_record->m_Tickrecords.at( index + 1 );
			else
				prev_record = nullptr;
		}

		if ( is_time_delta_too_large( current_record ) )
			continue;

		if ( has_knife || has_zeus ) {
			C_Tickrecord corrected_record;

			auto tick_count = start_lag_compensation( entity, index, &corrected_record );

			if ( tick_count == -1 || !corrected_record.data_filled )
				continue;

			if ( index > 0 && prev_record != nullptr && current_record->origin == prev_record->origin )
				continue;

			apply_record_data( entity, &corrected_record );

			auto chest = cheat::features::aimbot.get_hitbox( entity, HITBOX_CHEST, corrected_record.matrixes );
			auto dist = cheat::main::local( )->m_vecOrigin( ).Distance( chest );

			if ( dist < max_distance ) {
				max_distance = dist;

				player_record->tick_count = tick_count;
				player_record->type = corrected_record.type;
				memcpy( player_record->matrix, corrected_record.matrixes, sizeof( matrix3x4_t ) * 128 );
				player_record->matrix_valid = true;
				player_record->backtrack_ticks = index;
			}
		}
		else {
			C_Tickrecord corrected_record;

			auto tick_count = start_lag_compensation( entity, index, &corrected_record );

			if ( tick_count == -1 || !corrected_record.data_filled )
				continue;

			if ( index > 0 && prev_record != nullptr /*&& current_record->type <= RECORD_NORMAL*/ && /*current_record->velocity == prev_record->velocity && current_record->origin == prev_record->origin &&*/ current_record->hitbox_positon == prev_record->hitbox_positon )
				continue;

			if ( corrected_record.resolver_method != cheat::features::aaa.player_resolver_records[ entity->entindex( ) - 1 ].resolving_method && corrected_record.resolver_method != 1 && corrected_record.type != RECORD_SHOT )
				continue;

			apply_record_data( entity, &corrected_record );

			corrected_record.hitbox_positon = cheat::features::aimbot.get_hitbox( entity, 0, corrected_record.matrixes );
			auto chest = cheat::features::aimbot.get_hitbox( entity, HITBOX_CHEST, corrected_record.matrixes );

			if ( corrected_record.hitbox_positon.IsZero( ) || chest.IsZero( ) )
				continue;

			auto pass = cheat::features::aimbot.can_hit( 0, entity, corrected_record.hitbox_positon, corrected_record.matrixes, true );

			if ( pass <= 0.f )
				pass = cheat::features::aimbot.can_hit( HITBOX_CHEST, entity, chest, corrected_record.matrixes );

			player_record->hitbox_position = corrected_record.hitbox_positon;

			if ( pass > max_dmg || ( corrected_record.type > RECORD_NORMAL && pass > 0.f ) ) {
				player_record->tick_count = tick_count;
				player_record->type = corrected_record.type;
				memcpy( player_record->matrix, corrected_record.matrixes, sizeof( matrix3x4_t ) * 128 );
				player_record->matrix_valid = true;

				player_record->backtrack_ticks = index;
				max_dmg = pass;
				type = ( TickrecordType )( corrected_record.type );
			}

			if ( index == 0 && entity->m_vecVelocity( ).Length( ) > 100.f && pass > 0.f && corrected_record.type > RECORD_NORMAL )
				break;
		}
	}
}

void c_lagcomp::finish_position_adjustment( ) {
	if ( !cheat::Cvars.RageBot_AdjustPositions.GetValue( ) )
		return;

	for ( auto index = 1; index < Source::m_pGlobalVars->maxClients; index++ ) {
		auto entity = ( C_BasePlayer* )Source::m_pEntList->GetClientEntity( index );

		if ( !entity || !entity->IsPlayer( ) )
			continue;

		if ( entity->IsDormant( ) ||
			 entity->m_iHealth( ) <= 0 ||
			 entity->IsDead( ) ||
			 entity->m_iTeamNum( ) == cheat::main::local( )->m_iTeamNum( )
			 ) {
			auto entidx = entity->entindex( );

			if ( entidx == 0 )
				continue;

			records[ entidx - 1 ].reset( ( entity->IsDormant( ) && entity->m_iHealth( ) <= 0 ) );
			got_update[ entidx - 1 ] = false;
			continue;
		}

		finish_position_adjustment( entity );
	}
}

std::unordered_map <int, float> m_simulation;

void c_lagcomp::fix_netvar_compression( C_BasePlayer* player ) {
	auto& history = records[ player->entindex( ) - 1 ].m_Tickrecords;

	if ( history.size( ) < 2 || player == nullptr || player->get_animation_state( ) == nullptr )
		return;

	auto& record = history.front( );
	auto& pre_record = history.at( 1 );

	auto v15 = ( player->m_fFlags( ) & FL_ONGROUND ) == 0;

	float chokedTime = player->m_flSimulationTime( ) - record.simulation_time;
	//chokedTime = Math::clamp(chokedTime, fmaxf(Source::m_pGlobalVars->interval_per_tick, chokedTime), 1.0f);

	if ( chokedTime <= 1.0 )
		chokedTime = fmaxf( Source::m_pGlobalVars->interval_per_tick, chokedTime );
	else
		chokedTime = 0.0;

	Vector origin = player->m_vecOrigin( );
	Vector deltaOrigin = ( origin - record.origin ) * ( 1.0f / chokedTime );

	if ( v15 || !( record.entity_flags & FL_ONGROUND ) ) {

		float penultimateChoke = record.simulation_time - pre_record.simulation_time;
		//penultimateChoke = Math::clamp(penultimateChoke, 0.0f, fmaxf(Source::m_pGlobalVars->interval_per_tick, penultimateChoke));

		if ( penultimateChoke <= 1.0 )
			penultimateChoke = fmaxf( Source::m_pGlobalVars->interval_per_tick, penultimateChoke );
		else
			penultimateChoke = 0.0;

		float delta = RAD2DEG( atan2( ( record.origin.y - pre_record.origin.y ) * ( 1.0f / penultimateChoke ),
							   ( record.origin.x - pre_record.origin.x ) * ( 1.0f / penultimateChoke ) ) );

		float direction = RAD2DEG( atan2( deltaOrigin.y, deltaOrigin.x ) );
		float deltaDirection = Math::normalize_angle( direction - delta );

		deltaDirection = DEG2RAD( deltaDirection * 0.5f + direction );

		float dirCos, dirSin;

		dirSin = sin( deltaDirection );
		dirCos = cos( deltaDirection );

		float move = deltaOrigin.Length2D( );
		Vector velocity;
		velocity.x = dirCos * move;
		velocity.y = dirSin * move;
		velocity.z = deltaOrigin.z;

		if ( !( player->m_fFlags( ) & FL_ONGROUND ) || !player->get_animation_state( )->on_ground )
			velocity.z -= Source::m_pCvar->FindVar( "sv_gravity" )->GetFloat( ) * chokedTime * 0.5f;

		player->m_vecVelocity( ) = velocity;
	}
	else {
		player->m_vecVelocity( ) = deltaOrigin;
	}
}

void c_lagcomp::fix_poses( C_BasePlayer* m_player ) {
	auto idx = m_player->entindex( ) - 1;

	auto m_records = records[ idx ].m_Tickrecords;

	if ( m_records.empty( ) )
		return;

	auto resolver_info = &cheat::features::aaa.player_resolver_records[ idx ];

	auto flPitch = m_records[ 0 ].eye_angles.x;
	if ( m_records.size( ) > 2 && cheat::Cvars.RageBot_Resolver.GetValue( ) ) {
		if ( std::fabsf( flPitch - m_records[ 1 ].eye_angles.x ) > 10.f && std::fabsf( flPitch - m_records[ 2 ].eye_angles.x ) > 10.f ) {
			flPitch = m_records[ 1 ].eye_angles.x;

			const auto m_lag = TIME_TO_TICKS( m_player->m_flSimulationTime( ) - m_records[ 0 ].simulation_time );

			if ( m_player->m_vecVelocity( ).Length2D( ) > 0.1 && m_player->m_vecVelocity( ).Length2D( ) < 75.f || resolver_info->fakewalk || m_player->m_fFlags( ) & FL_DUCKING && m_player->m_vecVelocity( ).Length2D( ) > 20.f || m_lag > 12 )
				m_player->m_angEyeAngles( ).x = flPitch;
		}
	}

	if ( flPitch > 180.0f ) {
		flPitch -= 360.0f;
	}
	flPitch = std::clamp( flPitch, -90.f, 90.f );

	m_player->set_pose_param( BODY_PITCH, flPitch );
}

void c_lagcomp::update_animations_data( C_BasePlayer* m_player ) {
	CCSGOPlayerAnimState* state = m_player->get_animation_state( );

	auto idx = m_player->entindex( ) - 1;

	if ( !state || m_player == cheat::main::local( ) )
		return;

	auto weapon = ( C_WeaponCSBaseGun* )( Source::m_pEntList->GetClientEntityFromHandle( m_player->m_hActiveWeapon( ) ) );

	m_player->set_abs_origin( m_player->m_vecOrigin( ) );

	const auto flags_backup = m_player->m_fFlags( );
	const auto i_e_flags_backup = m_player->m_iEFlags( );
	const auto base_vel_backup = m_player->m_vecBaseVelocity( );
	const auto abs_origin_backup = m_player->get_abs_origin( );

	m_player->set_abs_origin( m_player->m_vecOrigin( ) );
	m_player->m_angEyeAngles( ).x = Math::NormalizePitch( m_player->m_angEyeAngles( ).x );

	auto m_records = records[ idx ].m_Tickrecords;
	auto resolver_info = &cheat::features::aaa.player_resolver_records[ idx ];

	const auto curtime = Source::m_pGlobalVars->curtime;
	const auto frametime = Source::m_pGlobalVars->frametime;
	auto time_delta = m_player->m_flSimulationTime( ) - m_player->m_flOldSimulationTime( );
	auto v75 = TIME_TO_TICKS( time_delta );

	if ( v75 >= 20 )
		Source::m_pGlobalVars->curtime = m_player->m_flSimulationTime( );
	else
		Source::m_pGlobalVars->curtime = Source::m_pGlobalVars->interval_per_tick + m_player->m_flOldSimulationTime( );

	Source::m_pGlobalVars->frametime = Source::m_pGlobalVars->interval_per_tick;

	memcpy( resolver_info->server_anim_layers, m_player->animation_layers_ptr( ), 0x38 * m_player->get_animation_layers_count( ) );
	resolver_info->m_flRate = resolver_info->server_anim_layers[ 6 ].m_flPlaybackRate;
	resolver_info->fakewalk = false;

	if ( m_records.size( ) > 0 && !m_player->IsBot( ) ) {
		int simulation_ticks = m_player->m_flSimulationTime( );
		int old_simulation_ticks = m_player->m_flOldSimulationTime( );
		const auto m_lag_time = Math::clamp( m_player->m_flSimulationTime( ) - m_player->m_flOldSimulationTime( ), Source::m_pGlobalVars->interval_per_tick, 1.0f );
		const auto m_lag = simulation_ticks - old_simulation_ticks;

		if ( m_player->m_fFlags( ) & FL_ONGROUND ) {
			if ( m_lag > 0 && m_lag <= 16 ) {
				m_player->m_vecVelocity( ) = ( m_player->m_vecOrigin( ) - m_records.front( ).origin ) * ( 1.f / TICKS_TO_TIME( m_lag ) );
			}
		}

		if ( m_lag > 0 ) {
			auto previous = &m_records[ 0 ];
			if ( previous && previous->data_filled ) {
				// estimate flags, duckamt, and animation velocity...
				m_player->m_fFlags( ) = previous->entity_flags;

				// strip the on ground flag.
				m_player->m_fFlags( ) &= ~FL_ONGROUND;

				// been onground for 2 consecutive ticks? fuck yeah.
				if ( flags_backup & FL_ONGROUND && previous->entity_flags & FL_ONGROUND )
					m_player->m_fFlags( ) |= FL_ONGROUND;

				// fix jump_fall.
				else {
					if ( resolver_info->server_anim_layers[ 4 ].m_flWeight != 1.f && previous->anim_layers[ 4 ].m_flWeight == 1.f && resolver_info->server_anim_layers[ 5 ].m_flWeight != 0.f )
						m_player->m_fFlags( ) |= FL_ONGROUND;

					if ( flags_backup & FL_ONGROUND && !( previous->entity_flags & FL_ONGROUND ) )
						m_player->m_fFlags( ) &= ~FL_ONGROUND;
				}

				// calc fraction between both records.
				float frac = ( m_player->m_anim_time( ) - m_player->m_flSimulationTime( ) ) / ( previous->simulation_time - m_player->m_flSimulationTime( ) );

				m_player->m_vecVelocity( ) = Math::Lerp( m_player->m_vecVelocity( ), previous->velocity, frac );

				if ( m_lag >= 2 )
					m_player->m_flDuckAmount( ) = Math::Lerp( m_player->m_flDuckAmount( ), previous->duck_amt, frac );
			}

			if ( resolver_info->server_anim_layers[ 6 ].m_flWeight < 0.1f && resolver_info->server_anim_layers[ 12 ].m_flWeight == 0.f && m_player->m_vecVelocity( ).Length2D( ) >= 0.1f && m_player->m_fFlags( ) & FL_ONGROUND ) {
				state->velocity = m_player->m_vecVelocity( ) = Vector::Zero;
				resolver_info->fakewalk = true;
				//m_player->m_bCanMoveDuringFreezePeriod() = false;
			}
		}
	}

	//state->on_ground = (m_player->m_fFlags() & FL_ONGROUND);

	//if (state->duck_amt > 0.0f)
	//	m_player->m_fFlags() |= FL_DUCKING;

	// EFL_DIRTY_ABSVELOCITY
	// skip call to C_BaseEntity::CalcAbsoluteVelocity
	m_player->m_iEFlags( ) &= ~0x1000;
	m_player->m_vecAbsVelocity( ) = m_player->m_vecVelocity( );

	cheat::features::aaa.resolve( m_player, cheat::main::shots_fired[ idx ], v75 );

	state->feet_rate = 0.f;

	if ( state ) {
		if ( state->last_anim_upd_tick == Source::m_pGlobalVars->framecount )
			state->last_anim_upd_tick = Source::m_pGlobalVars->framecount - 1;

		//if (state->last_anim_upd_time == Source::m_pGlobalVars->curtime)
		//	state->last_anim_upd_time = Source::m_pGlobalVars->curtime - state->anim_update_delta;
	}

	/*if (v75 > 1 && !m_records.empty())
	{
		auto old_duck_amount = m_records.front().duck_amt;

		m_player->m_flDuckAmount() = (((m_player->m_flDuckAmount() - old_duck_amount) / time_delta) * Source::m_pGlobalVars->interval_per_tick) + old_duck_amount;

		if (old_duck_amount < m_player->m_flDuckAmount() && m_player->m_flDuckAmount() != 1.f)
			m_player->m_angEyeAngles() = m_records.front().eye_angles;
	}*/

	m_player->update_clientside_animations( );

	fix_poses( m_player );

	memcpy( resolver_info->client_anim_layers, m_player->animation_layers_ptr( ), 0x38 * m_player->get_animation_layers_count( ) );
	resolver_info->m_flClientRate = resolver_info->client_anim_layers[ 6 ].m_flPlaybackRate;

	//*(QAngle*)(uintptr_t(m_player) + 0x128) = ang_rot_backup;

	memcpy( m_player->animation_layers_ptr( ), resolver_info->server_anim_layers, 0x38 * m_player->get_animation_layers_count( ) );

	Source::m_pGlobalVars->curtime = curtime; //local_weapon->GetCSWeaponData()->range
	Source::m_pGlobalVars->frametime = frametime;

	//m_player->m_bCanMoveDuringFreezePeriod() = can_move_during_assfuck;

	m_player->force_bone_rebuild( );
	m_player->SetupBonesEx( );

	//if (resolver_info->did_shot_this_tick) {
		//cheat::features::aimbot.visualise_hitboxes(m_player, m_player->m_CachedBoneData().Base(), Color::White(), Source::m_pGlobalVars->interval_per_tick * 2.0f);
		//m_player->DrawServerHitboxes();
	//}
}


void c_lagcomp::finish_position_adjustment( C_BasePlayer* entity ) {
	if ( entity == nullptr || !cheat::Cvars.RageBot_AdjustPositions.GetValue( ) )
		return;

	auto player_index = entity->entindex( ) - 1;
	auto player_record = &records[ player_index ];

	if ( !player_record->restore_record.data_filled )
		return;

	apply_record_data( entity, &player_record->restore_record, true );

	player_record->restore_record.data_filled = false; // Set to false so that we dont apply this record again if its not set next time
}

struct AnimationBackup_t {
	Vector m_origin;
	Vector m_abs_origin;
	Vector m_velocity;
	Vector m_abs_velocity;
	QAngle m_angles;
	int m_flags;
	int m_eflags;
	float m_duck;
	float m_body;
	std::array<C_AnimationLayer, 15> m_layers;
	std::array<float, 24u> m_poses;
};

void c_lagcomp::store_records( ) {
	get_interpolation( );

	if ( cheat::main::local( ) == nullptr ) {
		reset( );
		return;
	}

	for ( auto index = 1; index < Source::m_pGlobalVars->maxClients; index++ ) {
		auto entity = ( C_BasePlayer* )Source::m_pEntList->GetClientEntity( index );

		if ( !entity || !entity->IsPlayer( ) )
			continue;

		auto entidx = entity->entindex( );

		if ( entity->IsDormant( ) ||
			 entity->m_iHealth( ) <= 0 ||
			 entity->IsDead( ) ||
			 entity->m_iTeamNum( ) == cheat::main::local( )->m_iTeamNum( )
			 ) {
			records[ index - 1 ].reset( ( entity->IsDormant( ) && entity->m_iHealth( ) <= 0 ) );
			got_update[ index - 1 ] = false;

			continue;
		}

		if ( !entity->IsDormant( ) && entity->m_iTeamNum( ) != cheat::main::local( )->m_iTeamNum( ) ) {
			auto& data = cheat::features::aaa.player_resolver_records[ entidx - 1 ];

			if ( ( entity->m_flSimulationTime( ) - data.last_simtime ) > 1.f )
				cheat::main::shots_fired[ entidx - 1 ] = 0;
		}

		if ( !cheat::main::local( )->IsDead( ) ) {
			AnimationBackup_t backup;

			backup.m_origin = entity->m_vecOrigin( );
			backup.m_abs_origin = entity->get_abs_origin( );
			backup.m_velocity = entity->m_vecVelocity( );
			backup.m_abs_velocity = entity->m_vecAbsVelocity( );
			backup.m_flags = entity->m_fFlags( );
			backup.m_eflags = entity->m_iEFlags( );
			backup.m_duck = entity->m_flDuckAmount( );
			backup.m_body = entity->m_flLowerBodyYawTarget( );
			backup.m_angles = entity->m_angEyeAngles( );
			backup.m_poses = entity->m_flPoseParameter( );

			const auto lol = entity->m_flSimulationTime( );
			update_player_record_data( entity );
			entity->m_flSimulationTime( ) = lol;

			// restore backup data.
			entity->m_angEyeAngles( ) = backup.m_angles;
			entity->m_vecOrigin( ) = backup.m_origin;
			entity->m_vecVelocity( ) = backup.m_velocity;
			entity->m_vecAbsVelocity( ) = backup.m_abs_velocity;
			entity->m_fFlags( ) = backup.m_flags;
			entity->m_iEFlags( ) = backup.m_eflags;
			entity->m_flDuckAmount( ) = backup.m_duck;
			entity->m_flLowerBodyYawTarget( ) = backup.m_body;
			entity->set_abs_origin( backup.m_abs_origin );
			entity->m_flPoseParameter( ) = backup.m_poses;
		}
		else {
			cheat::main::updating_anims = true;
			entity->client_side_animation( ) = true;
		}

	}
}

void c_lagcomp::reset( ) {
	for ( auto i = 0; i < 64; i++ ) {
		records[ i ].reset( true );
		//cheat::features::aaa.move_logs[i].clear();
	}
}