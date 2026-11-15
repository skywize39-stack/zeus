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
#include "animation_state.hpp"

void c_player_animstate_rebuilt::setup_velocity( CCSGOPlayerAnimState* state, C_BasePlayer* player ) {
	// [COLLAPSED LOCAL DECLARATIONS. PRESS KEYPAD CTRL-"+" TO EXPAND]

	auto entindex = player->entindex( ) - 1;

	//if (!(player->m_iEFlags() & 0x1000))
		//CBaseEntity::CalcAbsoluteVelocity((float *)player);

	auto v7 = state->anim_update_delta * 2000.0;
	auto velocity = state->velocity;
	state->speed_2d = velocity.z;
	velocity.z = 0.0f;

	/*float leanspd = m_vecLastSetupLeanVelocity.LengthSqr();

	m_bIsAccelerating = velocity.Length2D() > leanspd;*/

	auto weapon = player->get_weapon( );

	float flMaxMovementSpeed = 260.0f;
	if ( weapon )
		flMaxMovementSpeed = std::fmax( weapon->GetMaxWeaponSpeed( ), 0.001f );

	state->speed_2d = std::fmin( state->velocity.Length( ), 260.0f );
	state->speed_norm = Math::clamp( state->speed_2d / flMaxMovementSpeed, 0.0f, 1.0f );
	state->feet_shit = state->speed_2d / ( flMaxMovementSpeed * 0.520f );
	state->feet_speed = state->speed_2d / ( flMaxMovementSpeed * 0.340f );

	auto m_flVelocityUnknown = *( float* )( uintptr_t( state ) + 0x2A4 );

	if ( state->feet_shit < 1.0f ) {
		if ( state->feet_shit < 0.5f ) {
			float vel = m_flVelocityUnknown;
			float delta = state->anim_update_delta * 60.0f;
			float newvel;
			if ( ( 80.0f - vel ) <= delta ) {
				if ( -delta <= ( 80.0f - vel ) )
					newvel = 80.0f;
				else
					newvel = vel - delta;
			}
			else {
				newvel = vel + delta;
			}
			m_flVelocityUnknown = newvel;
		}
	}
	else
		m_flVelocityUnknown = state->speed_2d;

	bool bWasMovingLastUpdate = false;
	bool bJustStartedMovingLastUpdate = false;
	if ( state->speed_2d <= 0.0f ) {
		state->t_since_started_moving = 0.0f;
		bWasMovingLastUpdate = state->t_since_stopped_moving <= 0.0f;
		state->t_since_stopped_moving += state->anim_update_delta;
	}
	else {
		state->t_since_stopped_moving = 0.0f;
		bJustStartedMovingLastUpdate = state->t_since_started_moving <= 0.0f;
		state->t_since_started_moving = state->anim_update_delta + state->t_since_started_moving;
	}

	state->old_abs_yaw = state->abs_yaw;

	auto v47 = Math::clamp( state->abs_yaw, -360.0f, 360.0f );
	auto v49 = Math::normalize_angle( Math::AngleDiff( state->eye_yaw, v47 ) );

	if ( state->feet_speed >= 0.0 )
		state->feet_speed = fminf( state->feet_speed, 1.0 );
	else
		state->feet_speed = 0.0;

	auto v54 = state->duck_amt;
	auto v55 = ( ( ( ( *( float* )( ( uintptr_t )state + 0x11C ) ) * -0.30000001 ) - 0.19999999 ) * state->feet_speed ) + 1.0f;
	if ( v54 > 0.0 ) {
		if ( state->feet_shit >= 0.0 )
			state->feet_shit = fminf( state->feet_shit, 1.0 );
		else
			state->feet_shit = 0.0;

		v55 += ( ( state->feet_shit * v54 ) * ( 0.5f - v55 ) );
	}

	auto v58 = *( float* )( ( uintptr_t )state + 0x334 ) * v55;
	auto v59 = *( float* )( ( uintptr_t )state + 0x330 ) * v55;

	if ( v49 <= v58 ) {
		if ( v59 > v49 )
			state->abs_yaw = fabs( v59 ) + state->eye_yaw;
	}
	else
		state->abs_yaw = state->eye_yaw - fabs( v58 );

	state->abs_yaw = Math::normalize_angle( state->abs_yaw );

	if ( state->speed_2d > 0.1 || fabs( state->speed_up ) > 100.0 ) {
		state->abs_yaw = cheat::features::antiaimbot.some_func(
			state->eye_yaw,
			state->abs_yaw,
			( ( ( *( float* )( ( uintptr_t )state + 0x11C ) ) * 20.0f ) + 30.0f )
			* state->anim_update_delta );

		last_time_lby_updated[ entindex ] = Source::m_pGlobalVars->curtime + 0.22000001;

		player->m_flLowerBodyYawTarget( ) = state->eye_yaw;
	}
	else {
		state->abs_yaw = cheat::features::antiaimbot.some_func(
			player->m_flLowerBodyYawTarget( ),
			state->abs_yaw,
			state->anim_update_delta * 100.0f );

		if ( Source::m_pGlobalVars->curtime > last_time_lby_updated[ entindex ] ) {
			auto v60 = Math::AngleDiff( player->m_flLowerBodyYawTarget( ), state->eye_yaw );

			if ( abs( v60 ) > 35.0 ) {
				last_time_lby_updated[ entindex ] = Source::m_pGlobalVars->curtime + 1.1;

				player->m_flLowerBodyYawTarget( ) = state->eye_yaw;
			}
		}
	}

	if ( state->speed_2d <= 1.0
		 && state->on_ground
		 && state->anim_update_delta > 0.0
		 && ( Math::AngleDiff( state->old_abs_yaw, state->abs_yaw ) / state->anim_update_delta ) > 120.0 ) {

		balance_adjust[ entindex ] = true;
	}
	else
		balance_adjust[ entindex ] = false;

	if ( state->speed_2d > 0.0 ) {
		float velAngle = ( atan2( -state->velocity.y, -state->velocity.x ) * 180.0f ) * ( 1.0f / M_PI );

		if ( velAngle < 0.0f )
			velAngle += 360.0f;

		state->vel_lean = Math::normalize_angle( Math::AngleDiff( velAngle, state->abs_yaw ) );
	}

	state->lean = Math::normalize_angle( Math::AngleDiff( state->vel_lean, state->torso_yaw ) );

	if ( bJustStartedMovingLastUpdate && state->feet_rate <= 0.0 ) {
		state->torso_yaw = state->vel_lean;
		// too many memecode below, i did not even want to paste it there.
	}
	else {
		if ( state->speed_2d > 0.1f ) {
			state->torso_yaw = state->vel_lean;
		}
		else {
			if ( state->feet_shit >= 0.0 )
				state->feet_shit = fminf( state->feet_shit, 1.0 );
			else
				state->feet_shit = 0.0;

			if ( state->feet_speed >= 0.0 )
				state->feet_speed = fminf( state->feet_speed, 1.0 );
			else
				state->feet_speed = 0.0;

			//dword_10ADA7F8 = 1075729671;

			auto v105 = ( ( state->feet_shit - state->feet_speed ) * state->duck_amt ) + state->feet_speed;
			auto v156 = Math::normalize_angle( ( ( ( v105 + 0.1f ) * state->vel_lean ) + state->torso_yaw ) );

			state->torso_yaw = v156;
		}
	}

	float eye_goalfeet_delta = Math::AngleDiff( state->eye_yaw - state->abs_yaw, 360.0f );
	float new_body_yaw_pose = 0.0f; //not initialized?

	if ( eye_goalfeet_delta < 0.0f || v58 == 0.0f ) {
		if ( v59 != 0.0f )
			new_body_yaw_pose = ( eye_goalfeet_delta / v59 ) * -58.0f;
	}
	else {
		new_body_yaw_pose = ( eye_goalfeet_delta / v58 ) * 58.0f;
	}

	player->m_flPoseParameter( )[ 11 ] = ( new_body_yaw_pose * 0.00833333f ) + 0.5f; // body yaw

	// move_yaw
	float move_yaw = ( player->m_flPoseParameter( )[ 7 ] * 360.0f - 180.0f ) - ( state->abs_yaw - state->old_abs_yaw );
	if ( move_yaw <= 180.0f ) {
		if ( move_yaw < -180.0f )
			move_yaw = move_yaw + 360.0f;
	}
	else
		move_yaw = move_yaw - 360.0f;

	player->m_flPoseParameter( )[ 7 ] = ( move_yaw / 120.f ) + 0.5f;
}