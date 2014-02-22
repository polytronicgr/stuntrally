#include "pch.h"
#include "../vdrift/par.h"
#include "common/Def_Str.h"
#include "CGame.h"
#include "CHud.h"
#include "CGui.h"
#include "common/data/SceneXml.h"
#include "common/CScene.h"
#include "FollowCamera.h"
#include "../road/Road.h"
#include "../vdrift/game.h"
#include "../vdrift/quickprof.h"
#include "../vdrift/dbl.h"
#include "../network/gameclient.hpp"
#include "../shiny/Main/Factory.hpp"
#include "common/Slider.h"
#include "SplitScreen.h"
#include <OgreCamera.h>
using namespace Ogre;


//  newPoses - Get new car pos from game
//  caution: called from GAME, 2nd thread, no Ogre stuff here
/// Todo: move arrow update and ChampionshipAdvance to updatePoses ...
//---------------------------------------------------------------------------------------------------------------
void App::newPoses(float time)  // time only for camera update
{
	if (!pGame || bLoading || pGame->cars.empty() /*|| carPoses.empty() || iCurPoses.empty()*/)
		return;
	PROFILER.beginBlock(".newPos ");

	double rplTime = pGame->timer.GetReplayTime(0);  // from start
	double lapTime = pGame->timer.GetPlayerTime(0);
	double rewTime = pSet->rpl_ghostrewind ? pGame->timer.GetRewindTimeGh(0) : lapTime;

	//  iterate through all car models and set new pos info (from vdrift sim or replay)
	CarModel* carM0 = carModels[0];
	for (int c = 0; c < carModels.size(); ++c)
	{
		CarModel* carM = carModels[c];
		bool bGhost = carM->isGhost();
		CAR* pCar = carM->pCar;

		PosInfo pi;  // new, to fill data


		//  car perf test  logic
		//--------------------------------
		if (bPerfTest && c==0)
			newPerfTest(time);


		//  play  get data from replay / ghost
		///-----------------------------------------------------------------------
		if (bGhost)
		{	///>>  track's ghost
			if (carM->isGhostTrk())
			{
				TrackFrame tf;       // par: sec after, 1st lap
				float lap1 = pGame->timer.GetCurrentLap(0) > 0 ? 2.f : 0.f;
				bool ok = ghtrk.GetFrame(rewTime + lap1, &tf);
				//  car
				Axes::toOgre(pi.pos, tf.pos);
				pi.rot = Axes::toOgre(tf.rot);
				pi.braking = tf.brake;
				pi.steer = tf.steer / 127.f;
				//pi.fboost = 0.f;  pi.speed = 0.f;  pi.percent = 0.f;
				//pi.fHitTime = 0.f;  pi.fParIntens = 0.f;  pi.fParVel = 0.f;
				//pi.vHitPos = Vector3::ZERO;  pi.vHitNorm = Vector3::UNIT_Y;

				//  wheels
				//dynamics.SetSteering(state.steer, pGame->GetSteerRange());  //peers can have other game settins..
				for (int w=0; w < 4; ++w)
				{
					MATHVECTOR<float,3> whP = carM->whPos[w];
					whP[2] += 0.05f;  // up
					tf.rot.RotateVector(whP);
					Axes::toOgre(pi.whPos[w], tf.pos + whP);

					if (w < 2)  // front steer
					{	float a = (pi.steer * carM->maxangle) * -PI_d/180.f;
						QUATERNION<float> q;  q.Rotate(a, 0,0,1);
						pi.whRot[w] = Axes::toOgreW(tf.rot * carM->qFixWh[w%2] * q);
					}else
						pi.whRot[w] = Axes::toOgreW(tf.rot * carM->qFixWh[w%2]);
				}
			}else  ///>>  ghost
			{
				ReplayFrame gf;
				bool ok = ghplay.GetFrame(rewTime, &gf, 0);

				pi.FromRpl(&gf);

				for (int w=0; w < 4; ++w)
					pi.whR[w] = replay.header.whR[c][w]; //
			}
		}
		else  ///>>  replay
		if (bRplPlay)
		{
			#ifdef DEBUG
			assert(c < frm.size());
			#endif
			ReplayFrame& rf = frm[c];  // frm also used in car.cpp for sounds
			bool ok = replay.GetFrame(rplTime, &rf, c);
			if (!ok)  pGame->timer.RestartReplay(0);  //at end

			pi.FromRpl(&rf);

			for (int w=0; w < 4; ++w)
				pi.whR[w] = replay.header.whR[c][w]; //
		}
		else  ///>>  sim, game  -  get data from vdrift
		if (pCar)
		{
			pi.FromCar(pCar);
			pi.percent = carM->trackPercent;
		}
		pi.bNew = true;
		

		//<<  rewind
		///-----------------------------------------------------------------------
		if (!bRplPlay && !pGame->pause && !bGhost && pCar)
		if (pCar->bRewind && pSet->game.rewind_type > 0)
		{	//  do rewind (go back)
			double& gtime = pGame->timer.GetRewindTime(c);
			gtime = std::max(0.0, gtime - time * gPar.rewindSpeed);
			double& ghtim = pGame->timer.GetRewindTimeGh(c);
			ghtim = std::max(0.0, ghtim - time * gPar.rewindSpeed);  //rewind ghost time too
			if (gPar.backTime)
			{	pGame->timer.Back(c, - time * gPar.rewindSpeed);
				ghost.DeleteFrames(0, ghtim);
			}
			RewindFrame rf;
			bool ok = rewind.GetFrame(gtime, &rf, c);

			pCar->SetPosRewind(rf.pos, rf.rot, rf.vel, rf.angvel);
			pCar->dynamics.fDamage = rf.fDamage;  // take damage back
			carModels[c]->First();
		}
		else if (c < 4)  // save data
		{
			const CARDYNAMICS& cd = pCar->dynamics;
			RewindFrame fr;
			fr.time = pGame->timer.GetRewindTime(c);
			
			fr.pos = cd.body.GetPosition();
			fr.rot = cd.body.GetOrientation();
			fr.vel = cd.GetVelocity();
			fr.angvel = cd.GetAngularVelocity();
			fr.fDamage = cd.fDamage;

			rewind.AddFrame(fr, c);  // rec rewind
		}
		
		//<<  record  save data
		///-----------------------------------------------------------------------
		if (pSet->rpl_rec && !pGame->pause && !bGhost && pCar && c < 4)
		{
			//static int ii = 0;
			//if (ii++ >= 0)	// 1 half game framerate
			//{	ii = 0;

				ReplayFrame fr;
				fr.time = rplTime;
				fr.percent = carM->trackPercent;

				fr.FromCar(pCar);
				
				replay.AddFrame(fr, c);  // rec replay
				if (c==0)  /// rec ghost lap
				{
					fr.time = lapTime;
					ghost.AddFrame(fr, 0);
				}
				
				if (gui->valRplName2)  // recorded info ..not here, in update
				{
					int size = replay.GetNumFrames() * sizeof(ReplayFrame);
					std::string s = fToStr( float(size)/1000000.f, 2,5);
					String ss = String( TR("#{RplRecTime}: ")) + CHud::StrTime(replay.GetTimeLength()) + TR("   #{RplSize}: ") + s + TR(" #{UnitMB}");
					gui->valRplName2->setCaption(ss);
				}
			//}
		}
		if (bRplPlay && gui->valRplName2)  gui->valRplName2->setCaption("");
		///-----------------------------------------------------------------------
		

		//  checkpoints, lap start
		//-----------------------------------------------------------------------
		if (bRplPlay || bGhost)   // dont check for replay or ghost
			carM->bWrongChk = false;
		else
		{
			///  arrow update  --------------------------------------
			SplineRoad* road = scn->road;
			if (pSet->check_arrow && carM->eType == CarModel::CT_LOCAL
			  && !bRplPlay && hud->arrow.node && road && road->mChks.size()>0)
			{
				//  set animation start to old orientation
				hud->arrow.qStart = hud->arrow.qCur;
				
				//  game start: no animation
				bool noAnim = carM->iNumChks == 0;
				
				//  get vector from camera to checkpoint
				Vector3 chkPos = road->mChks[std::max(0, std::min((int)road->mChks.size()-1, carM->iNextChk))].pos;

				//  last checkpoint, point to start pos
				if (carM->iNumChks == road->mChks.size())
					chkPos = carM->vStartPos;
				
				const Vector3& playerPos = pi.pos;
				Vector3 dir = chkPos - playerPos;  dir[1] = 0;  // only x and z rotation
				Quaternion quat = Vector3::UNIT_Z.getRotationTo(-dir);

				const bool valid = !quat.isNaN();
				if (valid)
				{	if (noAnim)  hud->arrow.qStart = quat;
					hud->arrow.qEnd = quat;
				
					//  calc angle towards cam
					Real angle = (hud->arrow.qCur.zAxis().dotProduct(carM->fCam->mCamera->getOrientation().zAxis())+1)/2.0f;

					//  set color in material (red for wrong dir)
					Vector3 col1 = angle * Vector3(0.0, 1.0, 0.0) + (1-angle) * Vector3(1.0, 0.0, 0.0);
					Vector3 col2 = angle * Vector3(0.0, 0.4, 0.0) + (1-angle) * Vector3(0.4, 0.0, 0.0);

					sh::Vector3* v1 = new sh::Vector3(col1.x, col1.y, col1.z);
					sh::Vector3* v2 = new sh::Vector3(col2.x, col2.y, col2.z);
					sh::Factory::getInstance().setSharedParameter("arrowColour1", sh::makeProperty <sh::Vector3>(v1));
					sh::Factory::getInstance().setSharedParameter("arrowColour2", sh::makeProperty <sh::Vector3>(v2));
				}
			}
			
			//----------------------------------------------------------------------------
			if (carM->bGetStPos)  // first pos is at start
			{	carM->bGetStPos = false;
				carM->matStPos.makeInverseTransform(pi.pos, Vector3::UNIT_SCALE, pi.rot);
				carM->ResetChecks();
			}
			if (road && !carM->bGetStPos)
			{
				//  start/finish box dist
				Vector4 carP(pi.pos.x,pi.pos.y,pi.pos.z,1);
				carM->vStDist = carM0->matStPos * carP;  // start pos from 1st car always
				carM->bInSt = abs(carM->vStDist.x) < road->vStBoxDim.x && 
					abs(carM->vStDist.y) < road->vStBoxDim.y && 
					abs(carM->vStDist.z) < road->vStBoxDim.z;
							
				carM->iInChk = -1;  carM->bWrongChk = false;
				int ncs = road->mChks.size();
				if (ncs > 0)
				{
					//  Finish  --------------------------------------
					if (carM->eType == CarModel::CT_LOCAL &&
						(carM->bInSt && carM->iNumChks == ncs && carM->iCurChk != -1))
					{
						///  Lap
						bool finished = (pGame->timer.GetCurrentLap(c) >= pSet->game.num_laps)
										&& (mClient || pSet->game.local_players > 1);  // multiplay or split
						bool best = finished ? false :  // dont inc laps when race over (in multiplayer or splitscreen mode)
							pGame->timer.Lap(c, 0,0, !finished, pSet->game.trackreverse);  //,boost_type?
						double timeCur = pGame->timer.GetPlayerTimeTot(c);

						//  Network notification, send: car id, lap time  ----
						if (mClient && c == 0 && !finished)
							mClient->lap(pGame->timer.GetCurrentLap(c), pGame->timer.GetLastLap(c));

						///  new best lap, save ghost
						if (!pSet->rpl_bestonly || best || gPar.backTime)
						if (c==0 && pSet->rpl_rec)  // for many, only 1st car
						{
							ghost.SaveFile(gui->GetGhostFile());  //,boost_type?
							ghplay.CopyFrom(ghost);
							isGhost2nd = false;  // hide 2nd ghost
						}
						ghost.Clear();
						
						carM->ResetChecks();
						
						//  restore boost fuel, each lap  ----
						if (pSet->game.boost_type == 1 && carM->pCar)
							carM->pCar->dynamics.boostFuel = gPar.boostFuelStart;

						//  damage decrease, each lap  ---
						if (pSet->game.damage_dec > 0.f)
							carM->pCar->dynamics.fDamage = std::max(0.f,
								carM->pCar->dynamics.fDamage - pSet->game.damage_dec);

						//  upd lap results ----
						carM->updLap = false;
						carM->fLapAlpha = 1.f;

						///  all laps
						finished = pGame->timer.GetCurrentLap(c) >= pSet->game.num_laps;
						if (finished && !mClient)
						{
							bool champ = pSet->game.champ_num >= 0, chall = pSet->game.chall_num >= 0;
							if (!champ && !chall)
							{
								if (carM->iWonPlace == 0)	//  split screen winner places
									carM->iWonPlace = carIdWin++;
							}
							else if (champ)
								gui->ChampionshipAdvance(timeCur);
							else
								gui->ChallengeAdvance(timeCur);
						}
					}
					
					//  checkpoints  --------------------------------------
					for (int i=0; i < ncs; ++i)
					{
						const CheckSphere& cs = road->mChks[i];
						Real d2 = pi.pos.squaredDistance(cs.pos);
						if (d2 < cs.r2)  // car in checkpoint
						{
							carM->iInChk = i;
							
							//\  loop camera change
							if (pSet->cam_loop_chng && carM->fCam &&
								cs.loop && (carM->iLoopChk == -1 || carM->iLoopChk != i))
							{
								carM->iLoopChk = i;
								if (carM->iLoopLastCam == -1)
								{
									carM->iLoopLastCam = carM->fCam->miCurrent;
									carM->fCam->setCamera(pSet->cam_in_loop);
								}else
								{	carM->fCam->setCamera(carM->iLoopLastCam);
									carM->iLoopLastCam = -1;
								}
							}
							//  next check
							if (i == carM->iNextChk && carM->iNumChks < ncs)
							{
								carM->iCurChk = i;  carM->iNumChks++;
								carM->timeAtCurChk = pGame->timer.GetPlayerTime(c);
								int ii = (pSet->game.trackreverse ? -1 : 1) * road->iDir;
								carM->iNextChk = (carM->iCurChk + ii + ncs) % ncs;
								carM->UpdNextCheck();
								//  save car pos and rot
								carM->pCar->SavePosAtCheck();
								carM->updTimes = true;
							}
							else
							if (carM->iInChk != carM->iCurChk)
								carM->bWrongChk = true;
							break;
						}
				}	}
		}	}

		
		///  store new pos info in queue  _________
		if (!(isFocGui || isTweak()) || mClient)  // dont if gui, but network always
		{
			int qn = (iCurPoses[c] + 1) % CarPosCnt;  // next index in queue
			carPoses[qn][c] = pi;
			//  update camera
			if (carM->fCam)
				carM->fCam->update(time, pi, &carPoses[qn][c], &pGame->collision);
			iCurPoses[c] = qn;  // atomic, set new index in queue
		}
	}
	PROFILER.endBlock(".newPos ");
}


//  updatePoses - Set car pos for Ogre nodes, update particles, trails
//---------------------------------------------------------------------------------------------------------------
void App::updatePoses(float time)
{
	if (carModels.empty())  return;
	PROFILER.beginBlock(".updPos ");
	
	//  Update all carmodels from their carPos
	const CarModel* playerCar = carModels.front();

	int cgh = -1;
	for (int c = 0; c < carModels.size(); ++c)
	{		
		CarModel* carM = carModels[c];
		if (!carM)  {
			PROFILER.endBlock(".updPos ");
			return;  }
		
		///  ghosts visibility  . . .
		//  hide when empty or near car
		bool bGhostCar = carM->eType == (isGhost2nd ? CarModel::CT_GHOST2 : CarModel::CT_GHOST),  // show only actual
			bGhTrkVis = carM->isGhostTrk() && ghtrk.GetTimeLength()>0 && pSet->rpl_trackghost,
			bGhostVis = ghplay.GetNumFrames()>0 && pSet->rpl_ghost;
		if (bGhostCar)  cgh = c;

		if (carM->isGhost())  // for all
		{
			bool loading = iLoad1stFrames >= 0;  // show during load ?..
			bool curVisible = carM->mbVisible;
			bool newVisible = bGhostVis && bGhostCar || bGhTrkVis;
			
			if (loading)
				carM->setVisible(true);  //!carM->isGhost());
			else
			{	//  hide ghost when close to player
				float d = carM->pMainNode->getPosition().squaredDistance(playerCar->pMainNode->getPosition());
				if (d < gPar.ghostHideDist)
					newVisible = false;

				if (carM->isGhostTrk() && cgh >= 0)  // hide track's ghost when near ghost
				{
					float d = carM->pMainNode->getPosition().squaredDistance(carModels[cgh]->pMainNode->getPosition());
					if (d < gPar.ghostHideDistTrk)
						newVisible = false;
				}
				if (curVisible == newVisible)
					carM->hideTime = 0.f;
				else
				{	carM->hideTime += time;  // change vis after delay
					if (carM->hideTime > gPar.ghostHideTime)
						carM->setVisible(newVisible);
				}
		}	}

		
		//  update car pos
		int q = iCurPoses[c];
		int cc = (c + iRplCarOfs) % carModels.size();  // replay offset, camera from other car
		int qq = iCurPoses[cc];
		carM->Update(carPoses[q][c], carPoses[qq][cc], time);
		

		//  nick text pos upd
		if (carM->pNickTxt && carM->pMainNode)
		{
			Camera* cam = playerCar->fCam->mCamera;  //above car 1m
			Vector3 p = hud->projectPoint(cam, carM->pMainNode->getPosition() + Vector3(0,1.f,0));
			p.x = p.x * mSplitMgr->mDims[0].width * 0.5f;  //1st viewport dims
			p.y = p.y * mSplitMgr->mDims[0].height * 0.5f;
			carM->pNickTxt->setPosition(p.x-40, p.y-16);  //center doesnt work
			carM->pNickTxt->setVisible(p.z > 0.f);
		}
	}
	
	///  Replay info
	if (bRplPlay && pGame->cars.size() > 0)
	{
		double pos = pGame->timer.GetPlayerTime(0);
		float len = replay.GetTimeLength();
		if (gui->valRplPerc)  gui->valRplPerc->setCaption(fToStr(pos/len*100.f, 1,4)+" %");
		if (gui->valRplCur)  gui->valRplCur->setCaption(CHud::StrTime(pos));
		if (gui->valRplLen)  gui->valRplLen->setCaption(CHud::StrTime(len));

		if (gui->slRplPos)
		{	float v = pos/len;  gui->slRplPos->setValue(v);  }
	}	
	
	
	///  objects - dynamic (props)  -------------------------------------------------------------
	for (int i=0; i < scn->sc->objects.size(); ++i)
	{
		Object& o = scn->sc->objects[i];
		if (o.ms)
		{
			btTransform tr, ofs;
			o.ms->getWorldTransform(tr);
			const btVector3& p = tr.getOrigin();
			const btQuaternion& q = tr.getRotation();
			o.pos[0] = p.x();  o.pos[1] = p.y();  o.pos[2] = p.z();
			o.rot[0] = q.x();  o.rot[1] = q.y();  o.rot[2] = q.z();  o.rot[3] = q.w();
			o.SetFromBlt();
		}
	}

	PROFILER.endBlock(".updPos ");
}
