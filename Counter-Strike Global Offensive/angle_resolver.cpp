#include "aimbot.hpp"
#include "source.hpp"
#include "entity.hpp"
#include "player.hpp"
#include "weapon.hpp"
#include "hooked.hpp"
#include "math.hpp"
#include "displacement.hpp"
#include "angle_resolver.hpp"
#include "lag_compensation.hpp"
#include "anti_aimbot.hpp"
#include <algorithm>
#include <numeric>
#include "rmenu.hpp"


bool c_resolver::has_fake( C_BasePlayer* entity ) {
	auto index = entity->entindex( ) - 1;
	auto player_lag_record = &cheat::features::lagcomp.records[ index ];

	if ( player_lag_record->m_Tickrecords.size( ) < 2 )
		return true;

	if ( fabs( player_lag_record->m_Tickrecords.front( ).simulation_time - player_lag_record->m_Tickrecords.at( 1 ).simulation_time ) == Source::m_pGlobalVars->interval_per_tick )
		return false;

	return true;
}

void c_resolver::CLBYRECORD::store( C_BasePlayer* entity ) {
	lby = entity->m_flLowerBodyYawTarget( );
	speed = entity->m_vecVelocity( ).Length( );
}

bool c_resolver::compare_delta( float v1, float v2, float Tolerance ) {
	v1 = Math::normalize_angle( v1 );
	v2 = Math::normalize_angle( v2 );

	if ( fabs( Math::AngleDiff( v1, v2 ) ) <= Tolerance )
		return true;

	return false;
}

float brutelist[ ] =
{
	119.f,
	180.f,
	81.f,
	180.f,
	-82.f,
};

float brutelist_shots[ ] =
{
	0.f,
	119.f,
	180.f,
	-119.f,
	180.f,
	-82.f,
};

std::unordered_map <int, float> old_static_yaw = {};
std::unordered_map <int, int> prev_rotation = {};

void c_resolver::on_real_angle_arrive( C_BasePlayer* m_player, resolver_records* resolve_data, float real_yaw ) {
	if ( fabs( resolve_data->last_shot_time - Source::m_pGlobalVars->realtime ) > 0.1f )
		resolve_data->latest_delta_used = Math::AngleDiff( resolve_data->latest_fake.y, real_yaw );

	//resolve_data->is_velocity_angle_fine	= (resolve_data->last_speed > 0.1f && fabs(Math::AngleDiff(real_yaw, resolve_data->last_velocity_angle)) < 45.f);
	resolve_data->had_fake =
		resolve_data->fakeangles = ( fabs( Math::AngleDiff( resolve_data->latest_fake.y, real_yaw ) ) > 5.f || fabs( Math::AngleDiff( resolve_data->latest_angles.y, real_yaw ) ) > 5.f );
}

void c_resolver::on_new_data_arrive( C_BasePlayer* m_player, resolver_records* resolve_data, CCSGOPlayerAnimState* state ) {
	auto history = cheat::main::shots_fired[ m_player->entindex( ) - 1 ];

	auto current_velocity_angle = RAD2DEG( atan2( m_player->m_vecVelocity( ).y, m_player->m_vecVelocity( ).x ) );
	resolve_data->balance_adjust = ( resolve_data->server_anim_layers[ 3 ].m_flWeight > 0.01f && m_player->get_sec_activity( resolve_data->server_anim_layers[ 3 ].m_nSequence ) == 979 && resolve_data->server_anim_layers[ 3 ].m_flCycle < 1.f );
	auto activity = m_player->get_sec_activity( resolve_data->server_anim_layers[ 1 ].m_nSequence );

	if ( resolve_data->balance_adjust )
		resolve_data->last_time_balanced = Source::m_pGlobalVars->realtime;

	resolve_data->did_shot_this_tick = ( m_player->get_weapon( ) != nullptr && m_player->get_weapon( )->m_flLastShotTime( ) != resolve_data->last_shot_time && ( m_player->m_angEyeAngles( ).x != resolve_data->previous_angles.x || fabs( m_player->m_angEyeAngles( ).x ) < 65 ) );//((activity >= ACT_CSGO_FIRE_PRIMARY && activity <= ACT_CSGO_FIRE_SECONDARY_OPT_2) && resolve_data->server_anim_layers[1].m_flWeight >= 0.01f && m_player->get_animation_layer(1).m_flCycle < 0.05f) || (m_player->get_weapon() && m_player->get_weapon()->m_Activity() == 208);
	resolve_data->tick_delta = Math::normalize_angle( m_player->m_angEyeAngles( ).y - resolve_data->latest_angles.y );
	resolve_data->lby_delta = Math::normalize_angle( m_player->m_flLowerBodyYawTarget( ) - m_player->m_angEyeAngles( ).y );
	resolve_data->last_velocity_angle = current_velocity_angle;
	resolve_data->last_speed = state->speed_2d;
	resolve_data->last_lby = m_player->m_flLowerBodyYawTarget( );
	resolve_data->origin = m_player->m_vecOrigin( );

	if ( state->speed_2d > 0.1f )
		resolve_data->last_time_moved = Source::m_pGlobalVars->realtime;
	if ( TIME_TO_TICKS( m_player->m_flSimulationTime( ) - m_player->m_flOldSimulationTime( ) ) > 1 )
		resolve_data->last_time_choked = Source::m_pGlobalVars->realtime;

	resolve_data->is_balancing = ( fabs( Source::m_pGlobalVars->realtime - resolve_data->last_time_balanced ) < 0.6f && fabs( resolve_data->lby_delta ) > 0.0001f );
	resolve_data->did_lby_update_fail = (/*resolve_data->abs_yaw_delta) > 0.0001f &&*/ fabs( resolve_data->lby_delta ) > 0.0001f );

	resolve_data->fakeangles = ( resolve_data->is_balancing || TIME_TO_TICKS( m_player->m_flSimulationTime( ) - resolve_data->last_simtime ) > 1 );

	if ( resolve_data->fakeangles )
		resolve_data->latest_angles_when_faked = resolve_data->latest_angles;
}

bool c_resolver::resolve_freestand( C_BasePlayer* m_player, resolver_records* resolve, CCSGOPlayerAnimState* state, float& step ) {
	const auto freestanding_record = resolve->freestanding_record;

	//const float at_target_yaw = Math::CalcAngle(cheat::main::local()->m_vecOrigin(), m_player->m_vecOrigin()).y;

	if ( freestanding_record.left_damage <= 0 && freestanding_record.right_damage <= 0 ) {
		if ( freestanding_record.right_fraction == freestanding_record.left_fraction )
			return false;

		if ( freestanding_record.right_fraction < freestanding_record.left_fraction /*|| (cheat::main::shots_fired[m_player->entindex() - 1] % 2 == 0 && cheat::main::shots_fired[m_player->entindex() - 1] > 0)*/ )
			step = -1.f;
		else
			step = 1.f;
	}
	else {
		if ( freestanding_record.left_damage > freestanding_record.right_damage /*|| (cheat::main::shots_fired[m_player->entindex()-1] % 2 == 0 && cheat::main::shots_fired[m_player->entindex() - 1] > 0)*/ )
			step = -1.f;
		else
			step = 1.f;
	}

	return true;
}

void c_resolver::add_new_lby_delta_record( resolver_records* resolve, float record ) {
	resolve->lby_deltas.emplace_front( record );
}

bool c_resolver::get_lby_delta_record( C_BasePlayer* m_player, resolver_records* resolve, float& returnval ) {
	auto& piska = resolve->lby_deltas;

	if ( piska.size( ) <= 1 )
		return false;

	auto ilogs = ( piska.size( ) - 1 );

	if ( ilogs > 12 )
		piska.resize( 12 );

	float total = 0.f;

	for ( auto siski : piska )
		total += abs( siski );

	auto final_delta = ( total / ilogs );
	if ( resolve->missed_shots[ 3 ] % 2 == 1 )
		final_delta *= -1.f;

	returnval = Math::normalize_angle( final_delta );

	return true;
}

bool c_resolver::get_yaw_by_walls( C_BasePlayer* entity, resolver_records* resolve, float& yaw ) {
	const auto freestanding_record = resolve->freestanding_record;

	const float at_target_yaw = Math::CalcAngle( cheat::main::local( )->m_vecOrigin( ), entity->m_vecOrigin( ) ).y;

	if ( freestanding_record.left_damage >= 20 && freestanding_record.right_damage >= 20 )
		return false;

	if ( freestanding_record.left_damage <= 0 && freestanding_record.right_damage <= 0 ) {
		if ( freestanding_record.right_fraction < freestanding_record.left_fraction )
			yaw = at_target_yaw + 125.f;
		else
			yaw = at_target_yaw - 73.f;
	}
	else {
		if ( freestanding_record.left_damage > freestanding_record.right_damage )
			yaw = at_target_yaw + 130.f;
		else
			yaw = at_target_yaw - 49.f;
	}

	return true;
}

void c_resolver::resolve( C_BasePlayer* m_player, int& history, int& chocked_ticks ) {
	auto state = m_player->get_animation_state( );

	auto idx = m_player->entindex( ) - 1;

	auto lag_data = &player_resolver_records[ idx ];
	auto history_data = cheat::features::lagcomp.records[ idx ].m_Tickrecords;

	lag_data->resolved_yaw = state->abs_yaw;
	lag_data->did_lby_flick = false;

	if ( ( lag_data->last_simtime - m_player->m_flSimulationTime( ) ) > 0.5f ) {
		lag_data->lby_deltas.clear( );

		if ( lag_data->origin.Distance( m_player->m_vecOrigin( ) ) > 16.f/* && fabs(Math::AngleDiff(lag_data->last_lby, m_player->m_flLowerBodyYawTarget())) > 20.f*/ ) {
			lag_data->last_move_lby_valid/*saved_off_delta = lag_data->move_lby*/ = false;
		}
	}

	lag_data->did_shot_this_tick = ( m_player->get_weapon( ) != nullptr && m_player->get_weapon( )->m_flLastShotTime( ) != lag_data->last_shot_time && ( m_player->m_angEyeAngles( ).x != lag_data->previous_angles.x || fabs( m_player->m_angEyeAngles( ).x ) < 65.f ) );//((activity >= ACT_CSGO_FIRE_PRIMARY && activity <= ACT_CSGO_FIRE_SECONDARY_OPT_2) && resolve_data->server_anim_layers[1].m_flWeight >= 0.01f && m_player->get_animation_layer(1).m_flCycle < 0.05f) || (m_player->get_weapon() && m_player->get_weapon()->m_Activity() == 208);
	lag_data->last_lby = m_player->m_flLowerBodyYawTarget( );
	if ( !cheat::Cvars.Misc_AntiUT.GetValue( ) && history_data.size( ) > 1 ) {
		auto record1 = history_data.at( 0 ), record2 = history_data.at( 1 );

		if ( !( m_player->m_fFlags( ) & FL_ONGROUND ) && record2.entity_flags & FL_ONGROUND && m_player->m_angEyeAngles( ).x >= 178.36304f )
			m_player->m_angEyeAngles( ).x = -89.f;
		else {
			if ( abs( m_player->m_angEyeAngles( ).x ) > 89.f )
				m_player->m_angEyeAngles( ).x = cheat::main::shots_fired[ idx ] % 4 != 3 ? 89.f : -89.f;
			else
				m_player->m_angEyeAngles( ).x = cheat::main::shots_fired[ idx ] % 4 != 3 ? 89.f : m_player->m_angEyeAngles( ).x;
		}
	}

	auto current_velocity_angle = RAD2DEG( atan2( m_player->m_vecVelocity( ).y, m_player->m_vecVelocity( ).x ) );

	float lby = m_player->m_flLowerBodyYawTarget( );

	float step = 0.0f;

	lag_data->resolving_method = 0;
	auto at_targ = Math::CalcAngle( cheat::main::local( )->m_vecOrigin( ), m_player->m_vecOrigin( ) ).y;
	auto delta_from_bw = Math::AngleDiff( at_targ + 180.f, state->abs_yaw );
	auto vdelta_from_bw = Math::AngleDiff( at_targ + 180.f, current_velocity_angle );
	auto is_backwards = fabs( delta_from_bw ) < 70.f;
	float start_angle = m_player->m_angEyeAngles( ).y;

	lag_data->latest_angles = m_player->m_angEyeAngles( );

	lag_data->latest_fake.y = state->abs_yaw;
	lag_data->previous_rotation = m_player->m_angEyeAngles( ).y;
	lag_data->previous_angles = m_player->m_angEyeAngles( );

	if ( !( m_player->m_fFlags( ) & FL_ONGROUND ) || m_player->m_vecVelocity( ).Length2D( ) > 0.15f )
		lag_data->next_lby_time = FLT_MAX;

	if ( fabs( Math::AngleDiff( lby, lag_data->prev_lby ) ) > 30.f && lag_data->prev_lby != FLT_MAX ) {
		lag_data->did_lby_flick = true;
		lag_data->next_lby_time = m_player->m_anim_time( ) + 1.1f;

		lag_data->prev_lby = lby;
	}
	else
		lag_data->prev_lby = lby;

	if ( ( lag_data->next_lby_time - m_player->m_anim_time( ) ) < 0.0f && lag_data->next_lby_time != FLT_MAX ) {
		lag_data->did_lby_flick = true;
		lag_data->next_lby_time = m_player->m_anim_time( ) + 1.1f;
	}

	if ( lag_data->did_shot_this_tick && lag_data->pre_shot_pitch != FLT_MAX )
		m_player->m_angEyeAngles( ).x = lag_data->pre_shot_pitch;

	if ( cheat::Cvars.RageBot_Resolver.GetValue( ) ) {
		const auto on_ground = m_player->m_fFlags( ) & FL_ONGROUND/* && lag_data->prev_flags & FL_ONGROUND*/;

		if ( !on_ground )
			m_player->m_angEyeAngles( ).y = lby + Math::normalize_angle( brutelist_shots[ cheat::main::shots_fired[ idx ] % 6 ] );
		else {
			if ( m_player->m_vecVelocity( ).Length2D( ) > 0.1f ) {
				lag_data->did_lby_flick = true;
				m_player->m_angEyeAngles( ).y = lby;
				lag_data->move_lby = lby;
				lag_data->never_saw_movement = false;
				lag_data->last_move_lby_valid = true;
				lag_data->was_moving = true;
				lag_data->origin = m_player->m_vecOrigin( );
				lag_data->resolving_method = 1;
				lag_data->last_time_moved = Source::m_pGlobalVars->realtime;
			}
			else {
				if ( lag_data->was_moving && lag_data->last_move_lby_valid ) {
					auto v41 = Math::normalize_angle( lag_data->move_lby - lby );

					if ( fabs( v41 ) >= 100.f ) {
						lag_data->old_lby = lby;
						lag_data->saved_off_delta = v41;
						lag_data->was_moving = false;
					}
				}

				if ( ( Source::m_pGlobalVars->realtime - lag_data->last_time_moved ) > 0.75f && lag_data->was_moving ) {
					lag_data->old_lby = lby;
					lag_data->was_moving = false;
				}

				/*else */if ( lag_data->did_lby_flick && lag_data->missed_shots[ 2 ] < 2 ) {
					m_player->m_flSimulationTime( ) = m_player->m_flOldSimulationTime( );
					m_player->m_angEyeAngles( ).y = lby;
					lag_data->resolving_method = 2;
				}
				else {
					if ( lag_data->never_saw_movement || !lag_data->last_move_lby_valid || lag_data->saved_off_delta > 1000.0f ) {
						lag_data->resolving_method = 3;
						if ( float yaw_resolved = 0.0f; get_yaw_by_walls( m_player, lag_data, yaw_resolved ) ) {
							m_player->m_angEyeAngles( ).y = Math::normalize_angle( yaw_resolved );
							lag_data->resolving_method = 4;
						}
						else
							m_player->m_angEyeAngles( ).y = m_player->m_flLowerBodyYawTarget( );
					}
					else {
						lag_data->resolving_method = 4;

						if ( lag_data->last_move_lby_valid )
							m_player->m_angEyeAngles( ).y = lag_data->move_lby;
						else {
							m_player->m_angEyeAngles( ).y = lby + lag_data->saved_off_delta;

							if ( lag_data->saved_off_delta == 0.0f )
								m_player->m_angEyeAngles( ).y += RandomFloat( -20.f, 20.f );// -20 ; 20
						}
					}

					m_player->m_angEyeAngles( ).y += Math::normalize_angle( brutelist_shots[ cheat::main::shots_fired[ idx ] % 6 ] );


				}
			}
		}
	}

	m_player->m_angEyeAngles( ).y = Math::normalize_angle( m_player->m_angEyeAngles( ).y );

	if ( lag_data->did_shot_this_tick && m_player->get_weapon( ) )
		lag_data->last_shot_time = m_player->get_weapon( )->m_flLastShotTime( );

	lag_data->last_simtime = m_player->m_flSimulationTime( );

	if ( !lag_data->did_shot_this_tick )
		lag_data->pre_shot_pitch = m_player->m_angEyeAngles( ).x;

	lag_data->prev_flags = m_player->m_fFlags( );
}

void c_resolver::work( ) {
	for ( auto idx = 1; idx < Source::m_pGlobalVars->maxClients; idx++ ) {
		auto entity = ( C_BasePlayer* )Source::m_pEntList->GetClientEntity( idx );

		if ( entity == nullptr ||
			 !entity->IsPlayer( ) )
			continue;

		if ( entity->m_iHealth( ) <= 0 ||
			 entity->m_iTeamNum( ) == cheat::main::local( )->m_iTeamNum( ) ) {

			cheat::main::shots_fired[ idx - 1 ] = 0;

			//if (!move_logs[idx-1].empty())
			//	move_logs[idx-1].clear();

			continue;
		}

		//resolve(entity, );
	}
}