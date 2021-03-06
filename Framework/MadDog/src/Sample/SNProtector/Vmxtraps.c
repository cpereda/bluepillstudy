#include "vmxtraps.h"

//Hide these variables' memory later. 
#define KILLAPP_TIMER 1000 //If verification failed, then after this amount of CR3 switches, the protected app. will be killed.
LONG LeftTimeToKill; //left CR3 switches to kill the protected app.
PBOOLEAN pRegState; //Hypervisor side software registration state.
PParameter AppParameter[2];
BOOLEAN bInstallProc[2];
HANDLE hProtectedApp,AppPID;
//PEPROCESS ProtectedApp;
BOOLEAN CanKillApp;

ULONG HostCR3,GuestCR3;
PALLOCATED_PAGE HypervisorDrvObjPage;
ULONG OpLock;

extern PDRIVER_OBJECT g_HypervisorDrvObj;
static BOOLEAN NTAPI VmxDispatchCpuid (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
);

static BOOLEAN NTAPI VmxDispatchVmxInstrDummy (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
);

static BOOLEAN NTAPI VmxDispatchINVD (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
);

static BOOLEAN NTAPI VmxDispatchMsrRead (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
);

static BOOLEAN NTAPI VmxDispatchMsrWrite (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
);

static BOOLEAN NTAPI VmxDispatchCrAccess (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
);

extern NTSTATUS NTAPI MmAPMSavePage (
  PHYSICAL_ADDRESS PhysicalAddress, //Machine Physical Address
  PVOID HostAddress, //Guest Physical Address
  PVOID GuestAddress, //Guest Virtual Address
  PAGE_ALLOCATION_TYPE AllocationType, 
  ULONG uNumberOfPages,
  ULONG Flags,
  PALLOCATED_PAGE *pAllocatedPage
);

//Forward Function Lookup
extern VOID NTAPI CmInitSpinLock (
  PULONG BpSpinLock
);

extern VOID NTAPI CmAcquireSpinLock (
  PULONG BpSpinLock
);

extern VOID NTAPI CmReleaseSpinLock (
  PULONG BpSpinLock
);


VOID NTAPI MmAPMSaveMultiPage (
  PVOID GuestAddress, //Guest Virtual Address
  ULONG uNumberOfPages,
  PALLOCATED_PAGE *pAllocatedPage
)
{
	ULONG i;
	PALLOCATED_PAGE Alloced = NULL;
	PHYSICAL_ADDRESS PagePA;

	for (i = 0; i < uNumberOfPages; i++) 
	{
		// save pages
		PagePA = MmGetPhysicalAddress (GuestAddress);
		MmAPMSavePage (
			PagePA, 
			GuestAddress, 
			GuestAddress, 
			i == 0 ? PAT_POOL : PAT_DONT_FREE, 
			uNumberOfPages, 
			0,
			&Alloced);

		if(i==0 && pAllocatedPage)
			*pAllocatedPage = Alloced;
		
		GuestAddress = (PUCHAR) GuestAddress + PAGE_SIZE;
	}

}

/**
 * effects: Register traps in this function
 * requires: <Cpu> is valid
 */
NTSTATUS NTAPI VmxRegisterTraps (
  PCPU Cpu
)
{//Finished
  NTSTATUS Status;
  PNBP_TRAP Trap;

  // used to set dummy handler for all VMX intercepts when we compile without nested support
  ULONG32 i, TableOfVmxExits[] = {
    EXIT_REASON_VMCALL,
    EXIT_REASON_VMCLEAR,
    EXIT_REASON_VMLAUNCH,
    EXIT_REASON_VMRESUME,
    EXIT_REASON_VMPTRLD,
    EXIT_REASON_VMPTRST,
    EXIT_REASON_VMREAD,
    EXIT_REASON_VMWRITE,
    EXIT_REASON_VMXON,
    EXIT_REASON_VMXOFF
  };
    Status = HvInitializeGeneralTrap ( //<----------------4.1 Finish
        Cpu, 
        EXIT_REASON_CPUID,
        FALSE,
        0, // length of the instruction, 0 means length need to be get from vmcs later. 
        VmxDispatchCpuid, //<----------------4.2 Finish
        &Trap,
		LAB_TAG);
  if (!NT_SUCCESS (Status)) 
  {
    Print(("VmxRegisterTraps(): Failed to register VmxDispatchCpuid with status 0x%08hX\n", Status));
    return Status;
  }
  MadDog_RegisterTrap (Cpu, Trap);//<----------------4.3//Finish

    Status = HvInitializeGeneralTrap (
        Cpu, 
        EXIT_REASON_MSR_READ, 
        FALSE,
        0, // length of the instruction, 0 means length need to be get from vmcs later. 
        VmxDispatchMsrRead, 
		//VmxDispatchVmxInstrDummy,
        &Trap,
		LAB_TAG);
  if (!NT_SUCCESS (Status)) 
  {
    Print(("VmxRegisterTraps(): Failed to register VmxDispatchMsrRead with status 0x%08hX\n", Status));
    return Status;
  }
  MadDog_RegisterTrap (Cpu, Trap);

  Status = HvInitializeGeneralTrap (
      Cpu, 
      EXIT_REASON_MSR_WRITE, 
      FALSE,
      0,   // length of the instruction, 0 means length need to be get from vmcs later. 
      VmxDispatchMsrWrite, 
	  //VmxDispatchVmxInstrDummy,
      &Trap,
	  LAB_TAG);
  if (!NT_SUCCESS (Status)) 
  {
    Print(("VmxRegisterTraps(): Failed to register VmxDispatchMsrWrite with status 0x%08hX\n", Status));
    return Status;
  }
  MadDog_RegisterTrap (Cpu, Trap);

	//Status = HvInitializeGeneralTrap (
	//	Cpu, 
	//	EXIT_REASON_CR_ACCESS, 
	//	FALSE,
	//	0,  // length of the instruction, 0 means length need to be get from vmcs later. 
	//	VmxDispatchCrAccess, 
	//	&Trap,
	//	LAB_TAG);

	//if (!NT_SUCCESS (Status)) 
	//{
	//	Print(("VmxRegisterTraps(): Failed to register VmxDispatchCrAccess with status 0x%08hX\n", Status));
	//	return Status;
	//}
	//MadDog_RegisterTrap (Cpu, Trap);

  Status = HvInitializeGeneralTrap (
      Cpu, 
      EXIT_REASON_INVD, 
      FALSE,
      0,  // length of the instruction, 0 means length need to be get from vmcs later. 
      VmxDispatchINVD, 
      &Trap,
	  LAB_TAG);
  if (!NT_SUCCESS (Status)) 
  {
    Print(("VmxRegisterTraps(): Failed to register VmxDispatchINVD with status 0x%08hX\n", Status));
    return Status;
  }
  MadDog_RegisterTrap (Cpu, Trap);

  Status = HvInitializeGeneralTrap (
      Cpu, 
      EXIT_REASON_DR_ACCESS, 
      FALSE,
      0,  // length of the instruction, 0 means length need to be get from vmcs later. 
      VmxDispatchVmxInstrDummy, 
      &Trap,
	  LAB_TAG);
  if (!NT_SUCCESS (Status)) 
  {
    Print(("VmxRegisterTraps(): Failed to register VmxDispatchINVD with status 0x%08hX\n", Status));
    return Status;
  }
  MadDog_RegisterTrap (Cpu, Trap);  

  Status = HvInitializeGeneralTrap (
      Cpu, 
      EXIT_REASON_EXCEPTION_NMI, 
      FALSE,
      0,  // length of the instruction, 0 means length need to be get from vmcs later. 
      VmxDispatchVmxInstrDummy,//VmxDispatchPageFault, 
      &Trap,
	  LAB_TAG);
  if (!NT_SUCCESS (Status)) 
  {
    Print(("VmxRegisterTraps(): Failed to register VmxDispatchPageFault with status 0x%08hX\n", Status));
    return Status;
  }
  MadDog_RegisterTrap (Cpu, Trap);

  // set dummy handler for all VMX intercepts if we compile without nested support
  for (i = 0; i < sizeof (TableOfVmxExits) / sizeof (ULONG32); i++) 
  {
      Status = HvInitializeGeneralTrap (
          Cpu, 
          TableOfVmxExits[i], 
          FALSE,
          0,    // length of the instruction, 0 means length need to be get from vmcs later. 
          VmxDispatchVmxInstrDummy, 
          &Trap,
		  LAB_TAG);
    if (!NT_SUCCESS (Status)) 
    {
      Print(("VmxRegisterTraps(): Failed to register VmxDispatchVmon with status 0x%08hX\n", Status));
      return Status;
    }
    MadDog_RegisterTrap (Cpu, Trap);
  }

  return STATUS_SUCCESS;
}


//+++++++++++++++++++++Static Functions++++++++++++++++++++++++
/**
 * effects: Defines the handler of the VM Exit Event which is caused by CPUID.
 * In this function we will return "Hello World!" by pass value through eax,ebx
 * and edx registers.
 */
static BOOLEAN NTAPI VmxDispatchCpuid (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)//Finished//same
{
	ULONG32 fn, eax, ebx, ecx, edx;
	ULONG inst_len;
	//ULONG GuestEdx,HostCR3,GuestCR3;
	PCHAR CorrectUserName,CorrectSN;
	OBJECT_ATTRIBUTES ObjectAttr;
	CLIENT_ID ClientId;
	NTSTATUS Status;

	if (!Cpu || !GuestRegs)
		return TRUE;
	fn = GuestRegs->eax;

	#if DEBUG_LEVEL>1
		Print(("Helloworld:VmxDispatchCpuid(): Passing in Value(Fn): 0x%x\n", fn));
	#endif

	inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
	if (Trap->RipDelta == 0)
		Trap->RipDelta = inst_len;

	if (fn == SNPROTECTOR_VERIFY) 
	{
		CorrectUserName = "ymk";
		CorrectSN = "111";
		Print(("SNPROTECTOR:Magic knock received: %p\n", SNPROTECTOR_VERIFY));
		//AppParameter[Cpu->ProcessorNumber] = (PParameter)(GuestRegs->edx);
		
		memcpy(AppParameter[Cpu->ProcessorNumber]->UserName,(&(GuestRegs->ebx)),4);
		memcpy(AppParameter[Cpu->ProcessorNumber]->SerialNumber,(&GuestRegs->ecx),4);

		if(!CanKillApp)
		{
			LeftTimeToKill = KILLAPP_TIMER; 
			CanKillApp = TRUE;
		}
		//Get the protected program pid.
		AppPID = PsGetCurrentProcessId();
		InitializeObjectAttributes( &ObjectAttr , NULL , 0 , NULL , NULL ); 
		//
		// pid of process
		//
		ClientId.UniqueProcess = AppPID;
		ClientId.UniqueThread  = (HANDLE)0;
		
		Status = ZwOpenProcess(&hProtectedApp, PROCESS_ALL_ACCESS, &ObjectAttr, &ClientId);
		if( !NT_SUCCESS(Status) )
		{
			KdPrint(("VmxDispatchCpuid:ZwOpenProcess failed with status : %08X\n" , Status ));
			return FALSE;
		}

		
		bInstallProc[Cpu->ProcessorNumber] = TRUE;
		
		//Begin Verify the <Username> and <SerialNumber>
		if(memcmp(AppParameter[Cpu->ProcessorNumber]->UserName, CorrectUserName,4) ||
			memcmp(AppParameter[Cpu->ProcessorNumber]->SerialNumber, CorrectSN,4))
		{
			*pRegState = FALSE; //Verification failed.
			GuestRegs->eax = 0;
		}
		else
		{
			*pRegState = TRUE; //Verification success.
			GuestRegs->eax = 1;
		}
		//MmMapGuestPages((PVOID)GuestRegs->edx,2);
		//MmMapGuestPages(AppParameter[Cpu->ProcessorNumber]->sUserName,2);
		//MmMapGuestPages(AppParameter[Cpu->ProcessorNumber]->sSerialNumber,2);

		return TRUE;
	}
	else if(fn == SNPROTECTOR_HIDEDRV)
	{
		
		HostCR3 = RegGetCr3();
		GuestCR3 = VmxRead(GUEST_CR3);

		CmInitSpinLock(&OpLock);
		CmAcquireSpinLock(&OpLock);
		__asm
		{
			mov eax,GuestCR3
			mov cr3,eax
		}

		MmAPMSaveMultiPage(g_HypervisorDrvObj->DriverStart,
			BYTES_TO_PAGES(g_HypervisorDrvObj->DriverSize),
			&HypervisorDrvObjPage);
		
		MmHidingStrategyHideGuestPages(g_HypervisorDrvObj->DriverStart,
			BYTES_TO_PAGES(g_HypervisorDrvObj->DriverSize));

		__asm
		{
			mov eax,HostCR3
			mov cr3,eax
		}
		CmReleaseSpinLock(&OpLock);
		
		return TRUE;
	}
	else if(fn == SNPROTECTOR_UNHIDEDRV)
	{
		HostCR3 = RegGetCr3();
		GuestCR3 = VmxRead(GUEST_CR3);

		CmInitSpinLock(&OpLock);
		CmAcquireSpinLock(&OpLock);
		__asm
		{
			mov eax,GuestCR3
			mov cr3,eax
		}
		
		MmHidingStrategyRevealHiddenPages(g_HypervisorDrvObj->DriverStart,
			BYTES_TO_PAGES(g_HypervisorDrvObj->DriverSize));

		__asm
		{
			mov eax,HostCR3
			mov cr3,eax
		}
		CmReleaseSpinLock(&OpLock);
		
		return TRUE;
	}



	ecx = (ULONG) GuestRegs->ecx;
	MadDog_GetCpuIdInfo (fn, &eax, &ebx, &ecx, &edx);
	GuestRegs->eax = eax;
	GuestRegs->ebx = ebx;
	GuestRegs->ecx = ecx;
	GuestRegs->edx = edx;

	//VmxDumpVmcs()();
	Print(("SNProtector:Missed Magic knock:EXIT_REASON_CPUID fn 0x%x 0x%x 0x%x 0x%x 0x%x \n", fn, eax, ebx, ecx, edx));
	return TRUE;
}

static BOOLEAN NTAPI VmxDispatchVmxInstrDummy (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
	ULONG32 inst_len;
	ULONG32 addr;

	if (!Cpu || !GuestRegs)
		return TRUE;
	Print(("VmxDispatchVminstructionDummy(): Nested virtualization not supported in this build!\n"));

	inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
	Trap->RipDelta = inst_len;

	addr = GUEST_RIP;
	Print(("VmxDispatchVminstructionDummy(): GUEST_RIP 0x%X: 0x%llX\n", addr, VmxRead (addr)));
	addr = VM_EXIT_INTR_INFO;
	Print(("VmxDispatchVminstructionDummy(): EXIT_INTR 0x%X: 0x%llX\n", addr, VmxRead (addr)));
	addr = EXIT_QUALIFICATION;
	Print(("VmxDispatchVminstructionDummy(): QUALIFICATION 0x%X: 0x%llX\n", addr, VmxRead (addr)));
	addr = EXCEPTION_BITMAP;
	Print(("VmxDispatchVminstructionDummy(): EXCEPTION_BITMAP 0x%X: 0x%llX\n", addr, VmxRead (addr)));

	//VmxWrite (GUEST_RFLAGS, VmxRead (GUEST_RFLAGS) & (~0x8d5) | 0x1 /* VMFailInvalid */ );
	return TRUE;
}

static BOOLEAN NTAPI VmxDispatchINVD (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
	ULONG inst_len;

	if (!Cpu || !GuestRegs)
		return TRUE;

	inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
	if (Trap->RipDelta == 0)
		Trap->RipDelta = inst_len;

	return TRUE;
}

static BOOLEAN NTAPI VmxDispatchMsrRead (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
	LARGE_INTEGER MsrValue;
	ULONG32 ecx;
	ULONG inst_len;

	if (!Cpu || !GuestRegs)
	return TRUE;

	inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
	if (Trap->RipDelta == 0)
		Trap->RipDelta = inst_len;

	ecx = GuestRegs->ecx;

	switch (ecx) 
	{
		case MSR_IA32_SYSENTER_CS:
			MsrValue.QuadPart = VmxRead (GUEST_SYSENTER_CS);
			break;
		case MSR_IA32_SYSENTER_ESP:
			MsrValue.QuadPart = VmxRead (GUEST_SYSENTER_ESP);
			break;
		case MSR_IA32_SYSENTER_EIP:
			MsrValue.QuadPart = VmxRead (GUEST_SYSENTER_EIP);
			Print(("VmxDispatchMsrRead(): Guest EIP: 0x%x read MSR_IA32_SYSENTER_EIP value: 0x%x \n", 
			VmxRead(GUEST_RIP), 
			MsrValue.QuadPart));
			break;
		case MSR_GS_BASE:
			MsrValue.QuadPart = VmxRead (GUEST_GS_BASE);
			break;
		case MSR_FS_BASE:
			MsrValue.QuadPart = VmxRead (GUEST_FS_BASE);
			break;
		case MSR_EFER:
			MsrValue.QuadPart = Cpu->Vmx.GuestEFER;
			//_KdPrint(("Guestip 0x%llx MSR_EFER Read 0x%llx 0x%llx \n",VmxRead(GUEST_RIP),ecx,MsrValue.QuadPart));
			break;
		default:
			if (ecx <= 0x1fff
			|| (ecx >= 0xC0000000 && ecx <= 0xC0001fff))
			{
				MsrValue.QuadPart = MsrRead (ecx);
			}
	}

	GuestRegs->eax = MsrValue.LowPart;
	GuestRegs->edx = MsrValue.HighPart;

	return TRUE;
}


static BOOLEAN NTAPI VmxDispatchMsrWrite (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
	LARGE_INTEGER MsrValue;
	ULONG32 ecx;
	ULONG inst_len;

	if (!Cpu || !GuestRegs)
	return TRUE;

	inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
	if (Trap->RipDelta == 0)
	Trap->RipDelta = inst_len;

	ecx = GuestRegs->ecx;

	MsrValue.LowPart = (ULONG32) GuestRegs->eax;
	MsrValue.HighPart = (ULONG32) GuestRegs->edx;

	switch (ecx) 
	{
		case MSR_IA32_SYSENTER_CS:
			VmxWrite (GUEST_SYSENTER_CS, MsrValue.QuadPart);
			break;
		case MSR_IA32_SYSENTER_ESP:
			VmxWrite (GUEST_SYSENTER_ESP, MsrValue.QuadPart);
			break;
		case MSR_IA32_SYSENTER_EIP:
			VmxWrite (GUEST_SYSENTER_EIP, MsrValue.QuadPart);
			Print(("VmxDispatchMsrRead(): Guest EIP: 0x%x want to write MSR_IA32_SYSENTER_EIP value: 0x%x \n", 
				VmxRead(GUEST_RIP), 
				MsrValue.QuadPart));
			break;
		case MSR_GS_BASE:
			VmxWrite (GUEST_GS_BASE, MsrValue.QuadPart);
			break;
		case MSR_FS_BASE:
			VmxWrite (GUEST_FS_BASE, MsrValue.QuadPart);
			break;
		case MSR_EFER:
			//_KdPrint(("Guestip 0x%llx MSR_EFER write 0x%llx 0x%llx\n",VmxRead(GUEST_RIP),ecx,MsrValue.QuadPart)); 
			Cpu->Vmx.GuestEFER = MsrValue.QuadPart;
			MsrWrite (MSR_EFER, (MsrValue.QuadPart) | EFER_LME);
			break;
		default:
			if (ecx <= 0x1fff
				|| (ecx >= 0xC0000000 && ecx <= 0xC0001fff))
			{
				MsrWrite (ecx, MsrValue.QuadPart);
			}
	}

	return TRUE;
}

static BOOLEAN NTAPI VmxDispatchCrAccess (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
    ULONG32 exit_qualification;
    ULONG32 gp, cr;
    ULONG value;
    ULONG inst_len;

    if (!Cpu || !GuestRegs)
        return TRUE;

#if DEBUG_LEVEL>2
    Print(("VmxDispatchCrAccess()\n"));
#endif

    inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
    if (Trap->RipDelta == 0)
        Trap->RipDelta = inst_len;

    //For MOV CR, the general-purpose register:
    //  0 = RAX
    //  1 = RCX
    //  2 = RDX
    //  3 = RBX
    //  4 = RSP
    //  5 = RBP
    //  6 = RSI
    //  7 = RDI
    //  8�C15 represent R8-R15, respectively (used only on processors that support
    //  Intel 64 architecture)
    exit_qualification = (ULONG32) VmxRead (EXIT_QUALIFICATION);
    gp = (exit_qualification & CONTROL_REG_ACCESS_REG) >> 8;
    cr = exit_qualification & CONTROL_REG_ACCESS_NUM;

#if DEBUG_LEVEL>1
    Print(("VmxDispatchCrAccess(): gp: 0x%x cr: 0x%x exit_qualification: 0x%x\n", gp, cr, exit_qualification));
#endif

    //Access type:
    //  0 = MOV to CR
    //  1 = MOV from CR
    //  2 = CLTS
    //  3 = LMSW
    switch (exit_qualification & CONTROL_REG_ACCESS_TYPE) 
    {
    case TYPE_MOV_TO_CR:
        /*if (cr == 0) 
        {
            if (!(*(((PULONG) GuestRegs) + gp) & X86_CR0_WP))
            {
                // someone want to turn off the write protect
#if DEBUG_LEVEL>1
                _KdPrint (("VmxDispatchCrAccess(): Turnoff WP, gp: 0x%x cr: 0x%x\n", gp, cr));
#endif
                return TRUE;
            }

            Cpu->Vmx.GuestCR0 = *(((PULONG) GuestRegs) + gp);
            VmxWrite (GUEST_CR0, Cpu->Vmx.GuestCR0);
            return TRUE;

//======================================================================
            if (Cpu->Vmx.GuestCR0 & X86_CR0_PG) //enable paging
            {
                //_KdPrint(("VmxDispatchCrAccess():paging\n"));
                VmxWrite (GUEST_CR3, Cpu->Vmx.GuestCR3);
                if (Cpu->Vmx.GuestEFER & EFER_LME)
                    Cpu->Vmx.GuestEFER |= EFER_LMA;
                else
                    Cpu->Vmx.GuestEFER &= ~EFER_LMA;
            } 
            else    //disable paging
            {
                //_KdPrint(("VmxDispatchCrAccess():disable paging\n"));                         
                Cpu->Vmx.GuestCR3 = VmxRead (GUEST_CR3);
                VmxWrite (GUEST_CR3, g_IdentityPageTableBasePhysicalAddress_Legacy.QuadPart);
                Cpu->Vmx.GuestEFER &= ~EFER_LMA;
            }
#ifdef _X86_
            VmxWrite (CR0_READ_SHADOW, Cpu->Vmx.GuestCR0 & X86_CR0_PG);
#else
            VmxWrite (CR0_READ_SHADOW, Cpu->Vmx.GuestCR0 & X86_CR0_PG);
#endif
            VmxUpdateGuestEfer (Cpu);
            return FALSE;
//======================================================================
        }*/

        if (cr == 3) 
        {
            Cpu->Vmx.GuestCR3 = *(((PULONG) GuestRegs) + gp);

            if (Cpu->Vmx.GuestCR0 & X86_CR0_PG)       //enable paging
            {
#if DEBUG_LEVEL>2
                Print(("VmxDispatchCrAccess(): TYPE_MOV_TO_CR cr3:0x%x\n", *(((PULONG64) GuestRegs) + gp)));
#endif
                VmxWrite (GUEST_CR3, Cpu->Vmx.GuestCR3);

            }
            return TRUE;
        }
        /*if (cr == 4) 
        {

            //if(debugmode)
            //_KdPrint(("VmxDispatchCrAccess(): TYPE_MOV_TO_CR Cpu->Vmx.GuestEFER:0x%x Cpu->Vmx.GuestCR0:0x%x cr4:0x%x\n",Cpu->Vmx.GuestEFER,Cpu->Vmx.GuestCR0,*(((PULONG64)GuestRegs)+gp)));
            //Nbp need enabele VMXE. so guest try to clear cr4_vmxe, it would be mask.
#ifdef _X86_
            Cpu->Vmx.GuestCR4 = *(((PULONG32) GuestRegs) + gp);
            VmxWrite (CR4_READ_SHADOW, Cpu->Vmx.GuestCR4 & (X86_CR4_VMXE | X86_CR4_PAE));
            VmxWrite (GUEST_CR4, Cpu->Vmx.GuestCR4 | X86_CR4_VMXE);

#else
            //VmxWrite(CR4_READ_SHADOW, (*(((PULONG64)GuestRegs)+gp)) & (X86_CR4_VMXE|X86_CR4_PAE|X86_CR4_PSE));
            VmxWrite (CR4_READ_SHADOW, (*(((PULONG64) GuestRegs) + gp)) & (X86_CR4_VMXE));

            Cpu->Vmx.GuestCR4 = *(((PULONG64) GuestRegs) + gp);
            VmxWrite (GUEST_CR4, (*(((PULONG64) GuestRegs) + gp)) | X86_CR4_VMXE);
#endif

            return FALSE;
        }*/
        break;
    case TYPE_MOV_FROM_CR:
        if (cr == 3) 
        {
            value = Cpu->Vmx.GuestCR3;
#if DEBUG_LEVEL>2
            Print(("VmxDispatchCrAccess(): TYPE_MOV_FROM_CR cr3:0x%x\n", value));
#endif
            *(((PULONG32) GuestRegs) + gp) = (ULONG32) value;

        }
        break;
    case TYPE_CLTS:
        break;
    case TYPE_LMSW:
        break;
    }

    return TRUE;
}

BOOLEAN NTAPI VmxDispatchTimerExpired (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
	ULONG inst_len;

	if (!Cpu || !GuestRegs)
		return TRUE;

	inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
	if (Trap->RipDelta == 0)
		Trap->RipDelta = inst_len;
	
	DbgPrint("Timer Bomb,HAHAHAHAHAHA!!");
	return TRUE;
}

BOOLEAN NTAPI PtVmxDispatchCR3Access (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PNBP_TRAP Trap,
  BOOLEAN WillBeAlsoHandledByGuestHv,
  ...
)
{
	ULONG32 exit_qualification;
    ULONG32 gp, cr;
    ULONG value;
    ULONG inst_len;
	NTSTATUS Status;

    if (!Cpu || !GuestRegs)
        return TRUE;

    inst_len = VmxRead (VM_EXIT_INSTRUCTION_LEN);
    if (Trap->RipDelta == 0)
        Trap->RipDelta = inst_len;

    //For MOV CR, the general-purpose register:
    //  0 = RAX
    //  1 = RCX
    //  2 = RDX
    //  3 = RBX
    //  4 = RSP
    //  5 = RBP
    //  6 = RSI
    //  7 = RDI
    //  8�C15 represent R8-R15, respectively (used only on processors that support
    //  Intel 64 architecture)
    exit_qualification = (ULONG32) VmxRead (EXIT_QUALIFICATION);
    gp = (exit_qualification & CONTROL_REG_ACCESS_REG) >> 8;
    //cr = exit_qualification & CONTROL_REG_ACCESS_NUM;

    //Access type:
    //  0 = MOV to CR
    //  1 = MOV from CR
    //  2 = CLTS
    //  3 = LMSW
	
	/*if(bInstallProc[Cpu->ProcessorNumber])
	{
		HvmPrint(("PtVmxDispatchCR3Access(): sUserName: %s \n", AppParameter[Cpu->ProcessorNumber]->UserName));
		HvmPrint(("PtVmxDispatchCR3Access(): sSerialNumber: %s \n", AppParameter[Cpu->ProcessorNumber]->SerialNumber));
	}*/
	
	if(bInstallProc[Cpu->ProcessorNumber] && CanKillApp)
	{
		if(LeftTimeToKill >0)
		{
			LeftTimeToKill--;
		}
		else if(LeftTimeToKill <=0)
		{
			LeftTimeToKill = KILLAPP_TIMER;	

			//HostCR3 = RegGetCr3();
			//GuestCR3 = VmxRead(GUEST_CR3);
			//__asm
			//{
			//	mov eax,GuestCR3
			//	mov cr3,eax
			//}
			//	Status = ZwTerminateProcess(hProtectedApp,STATUS_SUCCESS);	
			//	//Status = ZwClose( hProtectedApp );
			//__asm
			//{
			//	mov eax,HostCR3
			//	mov cr3,eax
			//}

			HvmPrint(("PtVmxDispatchCR3Access:Do sth. to protect the app.\n"));
			CanKillApp = FALSE;
		}	
	}
    switch (exit_qualification & CONTROL_REG_ACCESS_TYPE) 
    {
	case TYPE_MOV_TO_CR:

        //if (cr == 3) 
        {
            Cpu->Vmx.GuestCR3 = *(((PULONG) GuestRegs) + gp);

            if (Cpu->Vmx.GuestCR0 & X86_CR0_PG)       //enable paging
            {
                //HvmPrint(("VmxDispatchCrAccess(): TYPE_MOV_TO_CR cr3:0x%x\n", *(((PULONG64) GuestRegs) + gp)));
                VmxWrite (GUEST_CR3, Cpu->Vmx.GuestCR3);

            }
            return TRUE;
        }
        break;
    case TYPE_MOV_FROM_CR:
        //if (cr == 3) 
        {
            value = Cpu->Vmx.GuestCR3;
            //HvmPrint(("VmxDispatchCrAccess(): TYPE_MOV_FROM_CR cr3:0x%x\n", value));

            *(((PULONG32) GuestRegs) + gp) = (ULONG32) value;

        }
        break;
    }

    return TRUE;
}


