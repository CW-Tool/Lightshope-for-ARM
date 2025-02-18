/*
 * Copyright (C) 2008-2011 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Creature.h"
#include "CreatureGroups.h"
#include "ObjectMgr.h"
#include "MoveSpline.h"
#include "CreatureAI.h"

#define MAX_DESYNC 5.0f

CreatureGroupMember* CreatureGroup::AddMember(ObjectGuid guid, float followDist, float followAngle, uint32 memberFlags)
{
    if (guid == _leaderGuid)
        return nullptr;
    CreatureGroupMember*& member = _members[guid];
    if (!member)
        member = new CreatureGroupMember();
    member->followDistance = followDist;
    member->followAngle    = followAngle;
    member->memberFlags    = memberFlags;
    _options |= memberFlags;
    return member;
}

void CreatureGroup::OnMemberAttackStart(Creature* member, Unit *target)
{
    if (!(_options & OPTION_AGGRO_TOGETHER))
        return;

    for (const auto& itr : _members)
        if (itr.first != member->GetObjectGuid())
            MemberAssist(member->GetMap()->GetCreature(itr.first), target);

    if (member->GetObjectGuid() != GetLeaderGuid())
        MemberAssist(member->GetMap()->GetCreature(GetLeaderGuid()), target);
}

void CreatureGroup::OnMemberDied(Creature* member)
{
    if (_options & OPTION_INFORM_LEADER_ON_MEMBER_DIED)
    {
        if (member->GetObjectGuid() != GetLeaderGuid())
            if (Creature* groupLeader = member->GetMap()->GetCreature(GetLeaderGuid()))
                if (groupLeader->IsInWorld() && groupLeader->isAlive() && groupLeader->AI())
                    groupLeader->AI()->GroupMemberJustDied(member, false);
    }
    if (_options & OPTION_INFORM_MEMBERS_ON_ANY_DIED)
    {
        for (const auto& itr : _members)
            if (itr.first != member->GetObjectGuid())
                if (Creature* otherMember = member->GetMap()->GetCreature(itr.first))
                    if (otherMember->IsInWorld() && otherMember->isAlive() && otherMember->AI())
                        otherMember->AI()->GroupMemberJustDied(member, member->GetObjectGuid() == GetLeaderGuid());
    }
}

void CreatureGroup::OnLeaveCombat(Creature* member)
{
    bool masterEvade = member->GetObjectGuid() == GetLeaderGuid();
    if (_options & OPTION_EVADE_TOGETHER)
    {
        for (const auto& itr : _members)
            if (itr.first != member->GetObjectGuid())
                if (Creature* otherMember = member->GetMap()->GetCreature(itr.first))
                    if (otherMember->IsInWorld() && otherMember->isAlive() && otherMember->AI())
                        otherMember->AI()->EnterEvadeMode();

        if (member->GetObjectGuid() != GetLeaderGuid())
            if (Creature* otherMember = member->GetMap()->GetCreature(GetLeaderGuid()))
                if (otherMember->IsInWorld() && otherMember->isAlive() && otherMember->AI())
                {
                    masterEvade = true;
                    otherMember->AI()->EnterEvadeMode();
                }
    }
    if (_options & OPTION_RESPAWN_ALL_ON_ANY_EVADE ||
            (_options & OPTION_RESPAWN_ALL_ON_MASTER_EVADE && masterEvade))
        RespawnAll(member);
}

void CreatureGroup::OnRespawn(Creature* member)
{
    if (!(_options & OPTION_RESPAWN_TOGETHER))
        return;

    RespawnAll(member);
}

void CreatureGroup::RespawnAll(Creature* except)
{
    for (const auto& itr : _members)
        if (itr.first != except->GetObjectGuid())
            if (Creature* otherMember = except->GetMap()->GetCreature(itr.first))
                Respawn(otherMember, itr.second);

    if (except->GetObjectGuid() != GetLeaderGuid())
        if (Creature* otherMember = except->GetMap()->GetCreature(GetLeaderGuid()))
            Respawn(otherMember, nullptr);
}

void CreatureGroup::Respawn(Creature* member, CreatureGroupMember const* memberEntry /* = nullptr for leader */)
{
    // Prevent stack overflow (member->Respawn() can call CreatureGroup::Respawn, etc ...)
    if (_respawnGuard)
        return;

    _respawnGuard = true;
    if (member->IsInWorld() && member->GetRespawnTime() > time(nullptr))
    {
        if (memberEntry && memberEntry->memberFlags & OPTION_FORMATION_MOVE)
        {
            if (Unit* leader = member->GetMap()->GetUnit(GetLeaderGuid()))
            {
                float x, y, z;
                if (leader->isAlive() || leader->GetTypeId() != TYPEID_UNIT)
                    leader->GetPosition(x, y, z);
                else
                    leader->ToCreature()->GetRespawnCoord(x, y, z);
                memberEntry->ComputeRelativePosition(leader->GetOrientation(), x, y);
                member->UpdateGroundPositionZ(x, y, z);
                member->NearTeleportTo(x, y, z, leader->GetAngle(x, y) + M_PI);
            }
        }
        member->Respawn();
    }
    _respawnGuard = false;
}

void CreatureGroup::MemberAssist(Creature* member, Unit *target)
{
    if (!member || !member->IsInWorld())
        return;

    if (!member->isAlive())
        return;

    if (member->getVictim())
        return;

    if (member->AI())
    {
        member->SetNoCallAssistance(true);
        member->AI()->AttackedBy(target);
    }
}

void CreatureGroup::RemoveMember(ObjectGuid guid)
{
    std::map<ObjectGuid, CreatureGroupMember*>::iterator it = _members.find(guid);
    if (it != _members.end())
    {
        delete it->second;
        _members.erase(it);
    }
}

void CreatureGroup::DisbandGroup(Creature* pMember)
{
    if (Creature* pLeader = pMember->GetMap()->GetCreature(GetLeaderGuid()))
        pLeader->SetCreatureGroup(nullptr);

    for (auto& it : _members)
    {
        if (Creature* pOtherMember = pMember->GetMap()->GetCreature(it.first))
        {
            pOtherMember->SetCreatureGroup(nullptr);
            if (IsFormation() && pOtherMember->isAlive())
                pOtherMember->GetMotionMaster()->Initialize();
        }
            
        delete it.second;
    }

    _members.clear();
}

void CreatureGroup::DeleteFromDb()
{
    WorldDatabase.PExecute("DELETE FROM creature_groups WHERE leader_guid=%u", _leaderGuid.GetCounter());
}

void CreatureGroup::SaveToDb()
{
    DeleteFromDb();
    for (const auto& itr : _members)
        WorldDatabase.PExecute("INSERT INTO creature_groups SET leader_guid=%u, member_guid=%u, dist='%f', angle='%f', flags=%u",
                               _leaderGuid.GetCounter(), itr.first.GetCounter(), itr.second->followDistance, itr.second->followAngle, itr.second->memberFlags);
}

uint32 CreatureGroup::GetWaitTime(Creature* moving)
{
    for (const auto& itr : _members)
        if (Creature* curr = moving->GetMap()->GetCreature(itr.first))
            if (!curr->movespline->Finalized())
                return 1;
    return 0;
}

bool CreatureGroupMember::ComputeRelativePosition(float leaderAngle, float &x, float &y) const
{
    x = cos(followAngle + leaderAngle) * followDistance;
    y = sin(followAngle + leaderAngle) * followDistance;
    return true;
}

ObjectGuid CreatureGroupsManager::ConvertDBGuid(uint32 guidlow)
{
    if (CreatureData const* data = sObjectMgr.GetCreatureData(guidlow))
        return data->GetObjectGuid(guidlow);
    return ObjectGuid();
}

void CreatureGroupsManager::Load()
{
    uint32 oldMSTime = WorldTimer::getMSTime();

    // Memory leak, but we cannot delete the loaded groups, since pointer may be present at loaded creatures
    _groups.clear();

    std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `leader_guid`, `member_guid`, `dist`, `angle`, `flags` FROM `creature_groups` ORDER BY `leader_guid`"));

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outErrorDb(">>  Loaded 0 creature groups. DB table `creature_groups` is empty!");
        return;
    }

    uint32 count = 0;
    Field *fields;
    CreatureGroup *currentGroup = nullptr;
    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();
        fields = result->Fetch();

        //Load group member data
        ObjectGuid leaderGuid = ConvertDBGuid(fields[0].GetUInt32());
        ObjectGuid memberGuid = ConvertDBGuid(fields[1].GetUInt32());
        if (leaderGuid.IsEmpty())
        {
            if (!sObjectMgr.IsExistingCreatureGuid(fields[0].GetUInt32()))
                sLog.outErrorDb("CREATURE GROUPS: Bad leader guid %u", fields[0].GetUInt32());
        }
        else if (memberGuid.IsEmpty())
        {
            if (!sObjectMgr.IsExistingCreatureGuid(fields[1].GetUInt32()))
                sLog.outErrorDb("CREATURE GROUPS: Bad member guid %u", fields[1].GetUInt32());
        }
        else
        {
            if (!currentGroup || leaderGuid != currentGroup->GetLeaderGuid())
            {
                currentGroup = new CreatureGroup(leaderGuid);
                RegisterNewGroup(currentGroup);
            }
            currentGroup->AddMember(memberGuid, fields[2].GetFloat(), fields[3].GetFloat(), fields[4].GetUInt32());
            ++count;
        }
    }
    while (result->NextRow());

    sLog.outString();
    sLog.outString(">> Loaded %u creature groups in %u ms", count, WorldTimer::getMSTime() - oldMSTime);
}

void CreatureGroupsManager::LoadCreatureGroup(Creature* creature, CreatureGroup*& group)
{
    group = nullptr;
    if (!creature->HasStaticDBSpawnData())
        return;
    for (const auto& itr : _groups)
    {
        if (itr.first == creature->GetObjectGuid() || itr.second->ContainsGuid(creature->GetObjectGuid()))
        {
            group = itr.second;
            break;
        }
    }
}
