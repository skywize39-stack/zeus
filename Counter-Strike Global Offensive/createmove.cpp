#include "hooked.hpp"
#include "displacement.hpp"
#include "player.hpp"
#include "weapon.hpp"
#include "prediction.hpp"
#include "movement.hpp"
#include "aimbot.hpp"
#include "anti_aimbot.hpp"
#include "menu.h"
#include "game_movement.h"
#include "lag_compensation.hpp"
#include <intrin.h>
#include "rmenu.hpp"

void update_cursor_send( CUserCmd* cmd ) {
	static QAngle delta_viewangles{};
	QAngle delta = cmd->viewangles - delta_viewangles;

	static ConVar* sensitivity = Source::m_pCvar->FindVar( "sensitivity" );
	if ( delta.x != 0.f ) {
		static ConVar* m_pitch = Source::m_pCvar->FindVar( "m_pitch" );

		int final_dy = static_cast< int >( ( delta.x / m_pitch->GetFloat( ) ) / sensitivity->GetFloat( ) );
		if ( final_dy <= 32767 ) {
			if ( final_dy >= -32768 ) {
				if ( final_dy >= 1 || final_dy < 0 ) {
					if ( final_dy <= -1 || final_dy > 0 )
						final_dy = final_dy;
					else
						final_dy = -1;
				}
				else {
					final_dy = 1;
				}
			}
			else {
				final_dy = 32768;
			}
		}
		else {
			final_dy = 32767;
		}

		cmd->mousedy = static_cast< short >( final_dy );
	}

	if ( delta.y != 0.f ) {
		static ConVar* m_yaw = Source::m_pCvar->FindVar( "m_yaw" );

		int final_dx = static_cast< int >( ( delta.y / m_yaw->GetFloat( ) ) / sensitivity->GetFloat( ) );
		if ( final_dx <= 32767 ) {
			if ( final_dx >= -32768 ) {
				if ( final_dx >= 1 || final_dx < 0 ) {
					if ( final_dx <= -1 || final_dx > 0 )
						final_dx = final_dx;
					else
						final_dx = -1;
				}
				else {
					final_dx = 1;
				}
			}
			else {
				final_dx = 32768;
			}
		}
		else {
			final_dx = 32767;
		}

		cmd->mousedx = static_cast< short >( final_dx );
	}

	delta_viewangles = cmd->viewangles;
}

namespace Hooked {
	bool __stdcall CreateMove( float flInputSampleTime, CUserCmd* cmd ) {
		if ( !cmd || cmd->command_number == 0 || !Source::m_pEngine->IsInGame( ) )
			return false;

		auto& prediction = Engine::Prediction::Instance( );
		auto& movement = Engine::Movement::Instance( );

		bool send_packet = true;
		cheat::main::fakewalking = cheat::game::pressed_keys[ int( cheat::Cvars.anti_aim_slowwalk_key.GetValue( ) ) ] && cheat::Cvars.anti_aim_slowwalk_key.GetValue( ) > 0.f;
		
		/* we use those */
		cheat::features::antiaimbot.skip_lags_this_tick = false;
		cheat::features::antiaimbot.unchoke = false;

		update_cursor_send( cmd );
		movement->Begin( cmd, send_packet );

		cmd->buttons |= IN_BULLRUSH;

		if ( cheat::main::fast_autostop )
			cheat::features::aimbot.autostop( cmd, send_packet, ( C_WeaponCSBaseGun* )( Source::m_pEntList->GetClientEntityFromHandle( cheat::main::local( )->m_hActiveWeapon( ) ) ) );

		const auto valid = cheat::main::local( ) && cheat::main::local( )->GetClientClass( ) && !cheat::main::local( )->IsDead( ) && cheat::main::local( )->m_MoveType( ) != 10;
		if ( valid ) {
			cheat::features::antiaimbot.DoLBY( cmd, &send_packet );

			static bool did_fakewalk = false;
			if ( cheat::main::fakewalking ) {

				if ( !did_fakewalk ) {
					Engine::Movement::Instance( )->quick_stop( cmd );

					if ( cheat::main::local( )->m_vecVelocity( ).Length2D( ) <= 0.1f )
						did_fakewalk = true;
				}

				static int choked = 0;
				choked = choked > 7 ? 0 : choked + 1;
				cmd->forwardmove = choked < 2 || choked > 5 ? 0 : cmd->forwardmove;
				cmd->sidemove = choked < 2 || choked > 5 ? 0 : cmd->sidemove;
				send_packet = choked < 1;
			}
			else
				did_fakewalk = false;
		}

		prediction->Begin( cmd );

		if ( valid ) {
			cheat::game::last_cmd = cmd;

			auto weapon = ( C_WeaponCSBaseGun* )( Source::m_pEntList->GetClientEntityFromHandle( cheat::main::local( )->m_hActiveWeapon( ) ) );
			if ( weapon ) {				
				cheat::features::antiaimbot.simulate_lags( cmd, &send_packet );

				auto aimbot_worked = cheat::features::aimbot.work( cmd, send_packet );// || cheat::features::aimbot.knifebot_work(cmd, send_packet);
				auto can_shoot = ( aimbot_worked || ( ( cheat::game::pressed_keys[ 1 ] || cheat::game::pressed_keys[ 2 ] ) && !cheat::features::menu.menu_opened ) ) && weapon->m_iItemDefinitionIndex( ) == 64 && weapon->can_shoot( ) || weapon->m_iItemDefinitionIndex( ) != 64 && cmd->buttons & IN_ATTACK && weapon->can_shoot( );

				if ( can_shoot ) {
					weapon->UpdateAccuracyPenalty( );

					auto weapon_recoil_scale = Source::m_pCvar->FindVar( "weapon_recoil_scale" )->GetFloat( );
					if ( weapon_recoil_scale > 0.f && cheat::Cvars.RageBot_NoRecoil.GetValue( ) ) {
						cmd->viewangles -= cheat::main::local( )->m_aimPunchAngle( ) * weapon_recoil_scale;

						if ( !cheat::Cvars.Misc_AntiUT.GetValue( ) ) {
							cmd->viewangles.x = 180.f - cmd->viewangles.x;
							cmd->viewangles.y = cmd->viewangles.y + 180.f;
						}
					}

					cheat::main::last_shot_time_clientside = Source::m_pGlobalVars->realtime;
					cmd->viewangles.Normalize( );
				}
				else
					cheat::features::antiaimbot.work( cmd, &send_packet );
			}

			cheat::features::antiaimbot.do_exloits( cmd, send_packet );

			// handle this there
			if ( !g_tickshift.m_shifting ) {
				g_tickshift.handle_doubletap( cmd, send_packet );
			}
			else {
				send_packet = g_tickshift.m_tick_to_shift == 1;
				cmd->buttons &= ~IN_ATTACK;
			}

			movement->End( cmd );

			if ( cheat::Cvars.Misc_AntiUT.GetValue( ) )
				cmd->viewangles.Clamp( );
		}

		prediction->End( );

		if ( send_packet && cheat::main::update_cycle & CYCLE_PRE_UPDATE ) {
			cheat::main::update_cycle &= ~CYCLE_PRE_UPDATE;
			cheat::main::update_cycle |= CYCLE_UPDATE;
		}

		static auto wasonground = true;
		const auto player_flags = cheat::main::local( )->m_fFlags( );

		if ( !wasonground || !( cheat::main::local( )->m_fFlags( ) & FL_ONGROUND ) )
			cheat::main::local( )->m_fFlags( ) &= ~FL_ONGROUND;

		if ( !Source::m_pClientState->m_iChockedCommands )
			wasonground = player_flags & FL_ONGROUND;

		const auto inlandanim = cheat::main::local( )->get_animation_state( ) && cheat::main::local( )->get_animation_state( )->hitgr_anim;
		const bool onground = cheat::main::local( )->m_fFlags( ) & FL_ONGROUND;

		if ( inlandanim && onground && wasonground ) {
			cheat::features::antiaimbot.last_sent_angle.x = -10.f;
			cheat::main::fix_modify_eye_pos = true;
		}
		else
			cheat::main::fix_modify_eye_pos = false;

		cheat::main::local( )->m_fFlags( ) = player_flags;

		if ( send_packet && cheat::main::local( ) ) {
			cheat::features::antiaimbot.last_sent_origin = cheat::main::local( )->m_vecOrigin( );
			cheat::features::antiaimbot.last_sent_simtime = cheat::main::local( )->m_flSimulationTime( );
			cheat::features::antiaimbot.last_real_angle = cmd->viewangles;
		}

		if ( ( Source::m_pClientState->m_iChockedCommands >= cheat::Cvars.Misc_fakelag_value.GetValue( ) && !send_packet ) || Source::m_pClientState->m_iChockedCommands >= ( cheat::Cvars.Misc_fakelag_value.GetValue( ) + 1 ) /*|| cheat::features::antiaimbot.unchoke*/ )
			send_packet = true;

		cmd->buttons &= ~IN_MOVERIGHT;
		cmd->buttons &= ~IN_MOVELEFT;
		cmd->buttons &= ~IN_FORWARD;
		cmd->buttons &= ~IN_BACK;

		if ( cmd->forwardmove > 0.f )
			cmd->buttons |= IN_FORWARD;
		else if ( cmd->forwardmove < 0.f )
			cmd->buttons |= IN_BACK;

		if ( cmd->sidemove > 0.f )
			cmd->buttons |= IN_MOVERIGHT;
		else if ( cmd->sidemove < 0.f )
			cmd->buttons |= IN_MOVELEFT;

		if ( !Source::m_pClientStateSwap && Source::m_pClientState && ( CClientState* )( uint32_t( Source::m_pClientState ) + 8 ) ) {
			Source::m_pClientStateSwap = std::make_shared<Memory::VmtSwap>( ( CClientState* )( uint32_t( Source::m_pClientState ) + 8 ) );
			Source::m_pClientStateSwap->Hook( &Hooked::PacketStart, 5 );
		}

		if ( send_packet )
			cheat::main::command_numbers.emplace_front( cmd->command_number );
		else {
			if ( Source::m_pClientState != nullptr ) {
				auto net_channel = *reinterpret_cast< INetChannel** >( reinterpret_cast< uintptr_t >( Source::m_pClientState ) + 0x9C );
				if ( net_channel != nullptr && net_channel->m_nChokedPackets > 0 && !( net_channel->m_nChokedPackets % 4 ) ) {
					const auto current_choke = net_channel->m_nChokedPackets;
					net_channel->m_nChokedPackets = 0;
					Memory::VCall<int( __thiscall* )( INetChannel*, void* )>( net_channel, 48 )( net_channel, 0 );
					--net_channel->m_nOutSequenceNr;
					net_channel->m_nChokedPackets = current_choke;
				}
			}
		}

		while ( cheat::main::command_numbers.size( ) > int( 1.0f / Source::m_pGlobalVars->interval_per_tick ) ) {
			cheat::main::command_numbers.pop_back( );
		}

		if ( !send_packet )
			cheat::features::antiaimbot.last_sent_angle = cmd->viewangles;

		*reinterpret_cast< bool* >( *reinterpret_cast< uintptr_t* >( static_cast< char* >( _AddressOfReturnAddress( ) ) - 0x4 ) - 0x1C ) = send_packet;
		return false;
	}
}