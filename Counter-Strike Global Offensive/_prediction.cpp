#include "hooked.hpp"
#include "prediction.hpp"
#include "player.hpp"
#include "angle_resolver.hpp"
#include "lag_compensation.hpp"
#include "anti_aimbot.hpp"
#include <intrin.h>
#include "weapon.hpp"
#include <iostream>

namespace Hooked {
	void FixViewmodel( CUserCmd* cmd, bool restore ) {
		if ( cheat::main::local( )->IsDead( ) )
			return;

		static float cycleBackup = 0.0f;
		static bool weaponAnimation = false;

		auto viewmodel = Source::m_pEntList->GetClientEntityFromHandle( ( CBaseHandle )cheat::main::local( )->m_hViewModel( ) );

		if ( viewmodel ) {
			if ( restore ) {
				weaponAnimation = cmd->weaponselect > 0 || cmd->buttons & ( IN_ATTACK2 | IN_ATTACK );
				cycleBackup = viewmodel->m_flCycle( );
			}
			else if ( weaponAnimation && cheat::main::update_cycle == 0 ) {
				if ( viewmodel->m_flCycle( ) == 0.0f && cycleBackup > 0.0f )
					cheat::main::update_cycle |= CYCLE_PRE_UPDATE;
			}
		}
	}

	void __fastcall RunCommand( void* ecx, void* edx, C_BasePlayer* player, CUserCmd* ucmd, IMoveHelper* moveHelper ) {
		using Fn = void( __thiscall* )( void*, C_BasePlayer*, CUserCmd*, IMoveHelper* );
		static int old_tickbase = 0;
		const auto is_valid = player != nullptr && player == cheat::main::local( ) && !cheat::main::local( )->IsDead( );

		/* optimizations */
		if ( !is_valid ) {
			Source::m_pPredictionSwap->VCall<Fn>( Index::IPrediction::RunCommand )( ecx, player, ucmd, moveHelper );

			/* log */
			if ( !cheat::main::local( )->IsDead( ) ) /* we dont want to get spammed when local is dead */
				std::cout << "[RunCommand] optimization #1\n";

			return;
		}

		if ( cheat::main::local( ) != nullptr && player == cheat::main::local( ) ) {
			FixViewmodel( ucmd, true );

			if ( cheat::main::can_store_netvars ) {
				cheat::main::run_cmd_got_called = true;
			}

			cheat::main::last_velocity_modifier = cheat::main::local( )->m_flVelocityModifier( );
		}

		if ( Source::m_pPredictionSwap ) /* kyss */
			Source::m_pPredictionSwap->VCall<Fn>( Index::IPrediction::RunCommand )( ecx, player, ucmd, moveHelper );

		if ( cheat::main::local( ) != nullptr && player == cheat::main::local( ) ) {
			cheat::main::local( )->m_flVelocityModifier( ) = cheat::main::last_velocity_modifier;
			FixViewmodel( ucmd, false );

			if ( cheat::main::can_store_netvars && cheat::main::last_frame_stage == 4 )
				cheat::main::last_netvars_update_tick = ucmd->command_number;

			const auto weapon = cheat::main::local( )->get_weapon( );
			static float next_update_time = Source::m_pGlobalVars->interval_per_tick;

			if ( weapon != nullptr ) {
				static int old_activity = weapon->m_Activity( );
				const auto tickbase = player->m_nTickBase( ) - 1;
				auto activity = weapon->m_Activity( );

				if ( weapon->m_iItemDefinitionIndex( ) == 64 && !ucmd->hasbeenpredicted ) {
					if ( old_activity != activity && weapon->m_Activity( ) == 208 )
						old_tickbase = tickbase + 2;

					if ( weapon->m_Activity( ) == 208 && old_tickbase == tickbase )
						weapon->m_flPostponeFireReadyTime( ) = TICKS_TO_TIME( tickbase ) + 0.2f;
				}

				old_activity = activity;
			}
		}

		if ( Source::m_pMoveHelper != moveHelper )
			Source::m_pMoveHelper = moveHelper;

		/* l3d why dont you run this there bruther? */
		auto& prediction = Engine::Prediction::Instance( );
		prediction->OnRunCommand( player );
	}

	bool __stdcall InPrediction( ) {
		using Fn = bool( __thiscall* )( void* );
		static auto MST = ( void* )Memory::Scan( cheat::main::clientdll, "84 C0 74 17 8B 87" );
		static auto SBT = ( void* )Memory::Scan( cheat::main::clientdll, "84 C0 74 0A F3 0F 10 05 ? ? ? ? EB 05" );

		if ( _ReturnAddress( ) == MST ) {
			return true;
		}

		if ( _ReturnAddress( ) == SBT ) {
			return true;
		}

		if ( Source::m_pPredictionSwap )
			return Source::m_pPredictionSwap->VCall<Fn>( Index::IPrediction::InPrediction )( Source::m_pPrediction );
	}
}