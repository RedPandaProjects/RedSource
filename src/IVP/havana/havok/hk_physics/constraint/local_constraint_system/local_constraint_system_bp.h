#ifndef HK_PHYSICS_LOCAL_CONSTRAINT_SYSTEM_BP
#define HK_PHYSICS_LOCAL_CONSTRAINT_SYSTEM_BP

// IVP_EXPORT_PUBLIC

class hk_Local_Constraint_System_BP  //: public hk_Effector_BP 
{ 
	public:

		hk_real m_damp;
		hk_real m_tau;

	public:

		hk_Local_Constraint_System_BP()
			:	m_damp( 1.0f ),
				m_tau( 1.0f )
		{
			;
		}
};

#endif /* HK_PHYSICS_LOCAL_CONSTRAINT_SYSTEM_BP */

