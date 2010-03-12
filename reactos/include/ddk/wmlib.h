
#ifndef _WMILIB_
#define _WMILIB_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _WMIGUIDREGINFO {
  LPCGUID Guid;
  ULONG InstanceCount;
  ULONG Flags;
} WMIGUIDREGINFO, *PWMIGUIDREGINFO;

typedef enum _WMIENABLEDISABLECONTROL {
  WmiEventControl,
  WmiDataBlockControl
} WMIENABLEDISABLECONTROL, *PWMIENABLEDISABLECONTROL;

typedef enum _SYSCTL_IRP_DISPOSITION {
  IrpProcessed,
  IrpNotCompleted,
  IrpNotWmi,
  IrpForward
} SYSCTL_IRP_DISPOSITION, *PSYSCTL_IRP_DISPOSITION;

typedef
NTSTATUS
(NTAPI WMI_QUERY_REGINFO_CALLBACK)(
  IN OUT PDEVICE_OBJECT DeviceObject,
  IN OUT PULONG RegFlags,
  IN OUT PUNICODE_STRING InstanceName,
  IN OUT PUNICODE_STRING *RegistryPath OPTIONAL,
  IN OUT PUNICODE_STRING MofResourceName,
  OUT PDEVICE_OBJECT *Pdo OPTIONAL);

typedef WMI_QUERY_REGINFO_CALLBACK *PWMI_QUERY_REGINFO;

typedef
NTSTATUS
(NTAPI WMI_QUERY_DATABLOCK_CALLBACK)(
  IN OUT PDEVICE_OBJECT DeviceObject,
  IN OUT PIRP Irp,
  IN OUT ULONG GuidIndex,
  IN ULONG InstanceIndex,
  IN ULONG InstanceCount,
  OUT PULONG InstanceLengthArray OPTIONAL,
  IN ULONG BufferAvail,
  OUT PUCHAR Buffer OPTIONAL);

typedef WMI_QUERY_DATABLOCK_CALLBACK *PWMI_QUERY_DATABLOCK;

typedef
NTSTATUS
(NTAPI WMI_SET_DATABLOCK_CALLBACK)(
  IN OUT PDEVICE_OBJECT DeviceObject,
  IN OUT PIRP Irp,
  IN ULONG GuidIndex,
  IN ULONG InstanceIndex,
  IN ULONG BufferSize,
  IN  PUCHAR Buffer);

typedef WMI_SET_DATABLOCK_CALLBACK *PWMI_SET_DATABLOCK;

typedef
NTSTATUS
(NTAPI WMI_SET_DATAITEM_CALLBACK)(
  IN OUT PDEVICE_OBJECT DeviceObject,
  IN OUT PIRP Irp,
  IN ULONG GuidIndex,
  IN ULONG InstanceIndex,
  IN ULONG DataItemId,
  IN ULONG BufferSize,
  IN PUCHAR Buffer);

typedef WMI_SET_DATAITEM_CALLBACK *PWMI_SET_DATAITEM;

typedef
NTSTATUS
(NTAPI WMI_EXECUTE_METHOD_CALLBACK)(
  IN OUT PDEVICE_OBJECT DeviceObject,
  IN OUT PIRP Irp,
  IN ULONG GuidIndex,
  IN ULONG InstanceIndex,
  IN ULONG MethodId,
  IN ULONG InBufferSize,
  IN ULONG OutBufferSize,
  IN OUT PUCHAR Buffer);

typedef WMI_EXECUTE_METHOD_CALLBACK *PWMI_EXECUTE_METHOD;

typedef
NTSTATUS
(NTAPI WMI_FUNCTION_CONTROL_CALLBACK)(
  IN OUT PDEVICE_OBJECT DeviceObject,
  IN OUT PIRP Irp,
  IN ULONG GuidIndex,
  IN WMIENABLEDISABLECONTROL Function,
  IN BOOLEAN Enable);

typedef WMI_FUNCTION_CONTROL_CALLBACK *PWMI_FUNCTION_CONTROL;

typedef struct _WMILIB_CONTEXT {
  ULONG GuidCount;
  PWMIGUIDREGINFO GuidList;
  PWMI_QUERY_REGINFO QueryWmiRegInfo;
  PWMI_QUERY_DATABLOCK QueryWmiDataBlock;
  PWMI_SET_DATABLOCK SetWmiDataBlock;
  PWMI_SET_DATAITEM SetWmiDataItem;
  PWMI_EXECUTE_METHOD ExecuteWmiMethod;
  PWMI_FUNCTION_CONTROL WmiFunctionControl;
} WMILIB_CONTEXT, *PWMILIB_CONTEXT;

#if (NTDDI_VERSION >= NTDDI_WIN2K)
NTSTATUS
NTAPI
WmiSystemControl(
  IN PWMILIB_CONTEXT WmiLibInfo,
  IN PDEVICE_OBJECT DeviceObject,
  IN OUT PIRP Irp,
  OUT PSYSCTL_IRP_DISPOSITION IrpDisposition);
#endif

#ifdef __cplusplus
}
#endif

#endif /* !_WMILIB_ */

