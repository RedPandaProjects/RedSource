#include "StdAfx.h"

#include <LinearMath/btDebug.h>

#include "Physics.h"
#include "Physics_Environment.h"
#include "Physics_ObjectPairHash.h"
#include "Physics_CollisionSet.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar vphysics_bulletdebugoutput("vphysics_bulletdebugoutput", "0", FCVAR_ARCHIVE);

void btDebugMessage(const char *str) {
	if (vphysics_bulletdebugoutput.GetBool()) {
		Msg("%s", str);
	}
}

void btDebugWarning(const char *str) {
	Warning("%s", str);
}

/******************
* CLASS CPhysics
******************/
CPhysics::~CPhysics() {
#if defined(_DEBUG) && defined(_MSC_VER)
	// Probably not the place we should be doing this, but who cares.
	// This'll be called when vphysics is unloaded, and since we're (most likely) the only module with
	// memory debugging enabled, memory leaks should only correspond to our code.
	// Also convars "leak" 2 bytes of memory because this is being called before it should
	//_CrtDumpMemoryLeaks();
#endif
}

InitReturnVal_t CPhysics::Init() {
	InitReturnVal_t nRetVal = BaseClass::Init();
	if (nRetVal != INIT_OK) return nRetVal;

	// Hook up our debug output functions
	btSetDbgMsgFn(btDebugMessage);
	btSetDbgWarnFn(btDebugWarning);

	return INIT_OK;
}

void CPhysics::Shutdown() {
	BaseClass::Shutdown();
}

void *CPhysics::QueryInterface(const char *pInterfaceName) {
	CreateInterfaceFn func = Sys_GetFactoryThis();
	if (!func)
		return NULL;

	return func(pInterfaceName, NULL);
}

IPhysicsEnvironment *CPhysics::CreateEnvironment() {
	IPhysicsEnvironment *pEnvironment = new CPhysicsEnvironment;
	m_envList.AddToTail(pEnvironment);
	return pEnvironment;
}

void CPhysics::DestroyEnvironment(IPhysicsEnvironment *pEnvironment) {
	m_envList.FindAndRemove(pEnvironment);
	delete (CPhysicsEnvironment *)pEnvironment;
}

IPhysicsEnvironment *CPhysics::GetActiveEnvironmentByIndex(int index) {
	if (index < 0 || index >= m_envList.Count()) return NULL;
	return m_envList[index];
}

int CPhysics::GetActiveEnvironmentCount() {
	return m_envList.Count();
}

IPhysicsObjectPairHash *CPhysics::CreateObjectPairHash() {
	return new CPhysicsObjectPairHash();
}

void CPhysics::DestroyObjectPairHash(IPhysicsObjectPairHash *pHash) {
	delete (CPhysicsObjectPairHash *)pHash;
}

IPhysicsCollisionSet *CPhysics::FindOrCreateCollisionSet(unsigned int id, int maxElementCount) {
	if (m_colSetTable.Find(id) != m_colSetTable.InvalidHandle())
		return m_collisionSets[m_colSetTable.Element(m_colSetTable.Find(id))];

	CPhysicsCollisionSet *set = NULL;
	if (maxElementCount < sizeof(int) * 8) { // Limit of 32 because of the way this works internally
		set = ::CreateCollisionSet(maxElementCount);
		int vecId = m_collisionSets.AddToTail(set);

		m_colSetTable.Insert(id, vecId);
	}

	return set;
}

IPhysicsCollisionSet *CPhysics::FindCollisionSet(unsigned int id) {
	if (m_colSetTable.Find(id) != m_colSetTable.InvalidHandle())
		return m_collisionSets[m_colSetTable.Element(m_colSetTable.Find(id))];

	return NULL;
}

void CPhysics::DestroyAllCollisionSets() {
	for (int i = 0; i < m_collisionSets.Count(); i++)
		delete (CPhysicsCollisionSet *)m_collisionSets[i];

	m_collisionSets.RemoveAll();
	m_colSetTable.RemoveAll();
}

CPhysics g_Physics;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CPhysics, IPhysics, VPHYSICS_INTERFACE_VERSION, g_Physics);
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CPhysics, IPhysics32, "VPhysics032", g_Physics); // "Undocumented" way to determine if this is the newer vphysics or not.
