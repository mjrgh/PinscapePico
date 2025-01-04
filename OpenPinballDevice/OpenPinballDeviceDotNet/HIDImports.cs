// HIDImports.cs
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// Based on WiimoteLib by Brian Peek (https://github.com/BrianPeek/WiimoteLib)
//
// C# bindings to the native Win32 API functions related to HID access,
// mostly from hid.lib and setup32.lib, plus low-level file-access
// functions to directly read and write USB devices via their virtual
// file system presence.  Everything here is just directly imported
// from Win32 APIs, so documentation can be found in the Windows SDK
// documentation.  C# provides managed objects for just a few of the
// system structs used by these calls, which we use where possible.
// Windows system handles are generally treated as IntPtr, which is
// the C# convention for passing handles to unmanaged code.

using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Policy;
using Microsoft.Win32.SafeHandles;

namespace OpenPinballDeviceDotNet
{
	public class HIDImports
	{

		// Windows error codes used internally
		internal const int ERROR_SUCCESS = 0;
		internal const int ERROR_INSUFFICIENT_BUFFER = 122;
		internal const int ERROR_IO_PENDING = 997;

		// Flags controlling what is included in the device information set built by SetupDiGetClassDevs
		internal const int DIGCF_DEFAULT = 0x00000001;       // only valid with DIGCF_DEVICEINTERFACE
		internal const int DIGCF_PRESENT = 0x00000002;
		internal const int DIGCF_ALLCLASSES = 0x00000004;
		internal const int DIGCF_PROFILE = 0x00000008;
		internal const int DIGCF_DEVICEINTERFACE = 0x00000010;

		// Success status returned from some calls
		public const int HIDP_STATUS_SUCCESS = (0x11 << 16);

		// invalid handle value from CreateFile
		public static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

		[Flags]
		internal enum EFileAttributes : uint
		{
			Readonly = 0x00000001,
			Hidden = 0x00000002,
			System = 0x00000004,
			Directory = 0x00000010,
			Archive = 0x00000020,
			Device = 0x00000040,
			Normal = 0x00000080,
			Temporary = 0x00000100,
			SparseFile = 0x00000200,
			ReparsePoint = 0x00000400,
			Compressed = 0x00000800,
			Offline = 0x00001000,
			NotContentIndexed = 0x00002000,
			Encrypted = 0x00004000,
			Write_Through = 0x80000000,
			Overlapped = 0x40000000,
			NoBuffering = 0x20000000,
			RandomAccess = 0x10000000,
			SequentialScan = 0x08000000,
			DeleteOnClose = 0x04000000,
			BackupSemantics = 0x02000000,
			PosixSemantics = 0x01000000,
			OpenReparsePoint = 0x00200000,
			OpenNoRecall = 0x00100000,
			FirstPipeInstance = 0x00080000
		}

		[StructLayout(LayoutKind.Sequential)]
		internal struct SP_DEVINFO_DATA
		{
			public uint cbSize;
			public Guid ClassGuid;
			public uint DevInst;
			public IntPtr Reserved;
		}

		[StructLayout(LayoutKind.Sequential)]
		internal struct SP_DEVICE_INTERFACE_DATA
		{
			public int cbSize;
			public Guid InterfaceClassGuid;
			public int Flags;
			public IntPtr RESERVED;
		}

		[StructLayout(LayoutKind.Sequential, Pack = 1)]
		internal struct SP_DEVICE_INTERFACE_DETAIL_DATA
		{
			public UInt32 cbSize;
			[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
			public string DevicePath;
		}

		[StructLayout(LayoutKind.Sequential)]
		public struct HIDD_ATTRIBUTES
		{
			public int Size;
			public ushort VendorID;
			public ushort ProductID;
			public short VersionNumber;
		}

		[DllImport(@"hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal static extern void HidD_GetHidGuid(out Guid gHid);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal static extern Boolean HidD_GetAttributes(IntPtr HidDeviceObject, ref HIDD_ATTRIBUTES Attributes);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal extern static bool HidD_SetOutputReport(
			IntPtr HidDeviceObject,
			byte[] lpReportBuffer,
			uint ReportBufferLength);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal extern static bool HidD_GetInputReport(
			IntPtr HidDeviceObject,
			byte[] lpReportBuffer,
			uint ReportBufferLength);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal extern static bool HidD_GetProductString(
			IntPtr HidDeviceObject,
			byte[] Buffer,
			uint BufferLength);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal extern static bool HidD_GetSerialNumberString(
			IntPtr HidDeviceObject, 
			byte[] Buffer, 
			uint BufferLength);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal extern static bool HidD_GetManufacturerString(
			IntPtr HidDeviceObject,
			byte[] Buffer,
			uint BufferLength);

		[DllImport("hid.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal extern static bool HidD_GetIndexedString(
			IntPtr HidDeviceObject,
			UInt32 StringIndex,
			byte[] Buffer,
			uint BufferLength);

		[DllImport(@"setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal static extern IntPtr SetupDiGetClassDevs(
			ref Guid ClassGuid,
			[MarshalAs(UnmanagedType.LPTStr)] string? Enumerator,
			IntPtr hwndParent,
			UInt32 Flags
			);

		[DllImport(@"setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal static extern Boolean SetupDiEnumDeviceInterfaces(
			IntPtr hDevInfo,
			IntPtr devInfo,
			ref Guid interfaceClassGuid,
			UInt32 memberIndex,
			ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData
			);

		[DllImport(@"setupapi.dll", SetLastError = true)]
		internal static extern Boolean SetupDiGetDeviceInterfaceDetail(
			IntPtr hDevInfo,
			ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
			IntPtr deviceInterfaceDetailData,
			UInt32 deviceInterfaceDetailDataSize,
			out UInt32 requiredSize,
			IntPtr deviceInfoData);

		[DllImport(@"setupapi.dll", SetLastError = true)]
		internal static extern Boolean SetupDiGetDeviceInterfaceDetail(
			IntPtr hDevInfo,
			ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
			ref SP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData,
			UInt32 deviceInterfaceDetailDataSize,
			out UInt32 requiredSize,
			IntPtr deviceInfoData);

		[DllImport(@"setupapi.dll", SetLastError = true)]
		internal static extern Boolean SetupDiGetDeviceInterfaceDetail(
			IntPtr hDevInfo,
			ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
			ref SP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData,
			UInt32 deviceInterfaceDetailDataSize,
			out UInt32 requiredSize,
			out SP_DEVINFO_DATA deviceInfoData);

		[DllImport(@"setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
		internal static extern UInt16 SetupDiDestroyDeviceInfoList(IntPtr hDevInfo);

		[DllImport("Kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
		internal static extern IntPtr CreateFile(
			string fileName,
			[MarshalAs(UnmanagedType.U4)] UInt32 fileAccess,
			[MarshalAs(UnmanagedType.U4)] UInt32 fileShare,
			IntPtr securityAttributes,
			[MarshalAs(UnmanagedType.U4)] FileMode creationDisposition,
			[MarshalAs(UnmanagedType.U4)] EFileAttributes flags,
			IntPtr template);

		// CreateFile access modes
		public const uint GENERIC_READ =  0x80000000;
		public const uint GENERIC_WRITE = 0x40000000;
		public const uint GENERIC_READ_WRITE = GENERIC_READ | GENERIC_WRITE;

		// CreateFile share modes
		public const uint SHARE_NONE = 0;
		public const uint SHARE_READ = 1;
		public const uint SHARE_WRITE = 2;
		public const uint SHARE_READ_WRITE = SHARE_READ | SHARE_WRITE;
		public const uint SHARE_DELETE = 4;

		[DllImport("Kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
		internal static extern int WriteFile(
			IntPtr hFile,
			byte[] lpBuffer,
			UInt32 bytesToWrite,
			out UInt32 bytesWritten,
			ref System.Threading.NativeOverlapped lpOverlapped);

		[DllImport("Kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
		[return: MarshalAs(UnmanagedType.Bool)]
		internal static extern bool LockFile(
			IntPtr hFile,
			UInt32 dwFileOffsetLow,
			UInt32 dwFileOffsetHigh,
			UInt32 nNumberOfBytesToLockLow,
			UInt32 nNumberOfBytesToLockHigh);

		[DllImport("Kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
		internal static extern int ReadFile(
			IntPtr hFile,
			byte[] lpBuffer,
			UInt32 bytesToRead,
			out UInt32 bytesRead,
			ref System.Threading.NativeOverlapped lpOverlapped);

		[DllImport("Kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
		[return: MarshalAs(UnmanagedType.Bool)]
		internal static extern bool GetOverlappedResultEx(
			IntPtr hFile,
			ref System.Threading.NativeOverlapped lpOverlapped,
			out UInt32 lpNumberOfBytesTransferred,
			UInt32 dwMilliseconds,
			bool bAlertable);

		[DllImport("kernel32.dll", SetLastError = true)]
		[return: MarshalAs(UnmanagedType.Bool)]
		public static extern bool CloseHandle(IntPtr hObject);

		[StructLayout(LayoutKind.Sequential)]
		internal struct HIDP_PREPARSED_DATA
		{
			public int cbSize;
			public Guid InterfaceClassGuid;
			public int Flags;
			public IntPtr RESERVED;
		}

		[StructLayout(LayoutKind.Sequential)]
		internal struct HIDP_CAPS
		{
			public ushort Usage;
			public ushort UsagePage;
			public ushort InputReportByteLength;
			public ushort OutputReportByteLength;
			public ushort FeatureReportByteLength;
			public ushort Reserved0;
			public ushort Reserved1;
			public ushort Reserved2;
			public ushort Reserved3;
			public ushort Reserved4;
			public ushort Reserved5;
			public ushort Reserved6;
			public ushort Reserved7;
			public ushort Reserved8;
			public ushort Reserved9;
			public ushort Reserved10;
			public ushort Reserved11;
			public ushort Reserved12;
			public ushort Reserved13;
			public ushort Reserved14;
			public ushort Reserved15;
			public ushort Reserved16;
			public ushort NumberLinkCollectionNodes;
			public ushort NumberInputButtonCaps;
			public ushort NumberInputValueCaps;
			public ushort NumberInputDataIndices;
			public ushort NumberOutputButtonCaps;
			public ushort NumberOutputValueCaps;
			public ushort NumberOutputDataIndices;
			public ushort NumberFeatureButtonCaps;
			public ushort NumberFeatureValueCaps;
			public ushort NumberFeatureDataIndices;
		}

		[StructLayout(LayoutKind.Sequential)]
		internal struct HIDP_BUTTON_CAPS
		{
			public ushort UsagePage;
			public byte ReportID;
			public byte IsAlias;
			public ushort BitField;
			public ushort LinkCollection;
			public ushort LinkUsage;
			public ushort LinkUsagePage;
			public byte IsRange;           // true -> UsageMin/UsageMax and DataIndexMin/DataIndexMax give the ranges; false -> UsageMin and DataIndexMin are the single usage/index values
			public byte IsStringRange;     // true -> StringMin/StringMax give the range; false -> StringMin is the single string index value
			public byte IsDesignatorRange; // true -> DesignatorMin/DesignatorMax give the range; false -> DesignatorMin is the single value
			public byte IsAbsolute;
			public ushort ReportCount;
			public ushort Reserved2;

			// note - in the original structure, this is a ULONG[9] array
			public UInt32 Reserved3;
			public UInt32 Reserved4;
			public UInt32 Reserved5;
			public UInt32 Reserved6;
			public UInt32 Reserved7;
			public UInt32 Reserved8;
			public UInt32 Reserved9;
			public UInt32 Reserved10;
			public UInt32 Reserved11;

			// Note - in the original structure, the remaining members are
			// enclosed in a union with 'Range' and 'NotRange' overlays.
			// in the NotRange overlay, each Min/Max pair is a value/Reserved
			// pair.  So if we look at it as a flat struct, when a range is
			// not in effect, the Min contains the single value, and the Max
			// is unused.
			public ushort UsageMin;      // lower bound of usage range, or single usage value if !IsRange
			public ushort UsageMax;
			public ushort StringMin;     // lower bound of string index range, or single string index if !IsStringRange
			public ushort StringMax;
			public ushort DesignatorMin; // lower bound of designator range, or single designator if !IsDesignatorRange
			public ushort DesignatorMax;
			public ushort DataIndexMin;  // lower bound of data index range, or single data index if !IsRange
			public ushort DataIndexMax;
		}

		[DllImport("Hid.dll", SetLastError = true, CharSet = CharSet.Auto)]
		internal static extern bool HidD_GetPreparsedData(
			IntPtr hFile,
			out IntPtr pPreparsedData);

		[DllImport("Hid.dll", SetLastError = true, CharSet = CharSet.Auto)]
		internal static extern bool HidD_FreePreparsedData(
			IntPtr preparsedData);

		[DllImport("Hid.dll", SetLastError = true, CharSet = CharSet.Auto)]
		internal static extern int HidP_GetCaps(
			IntPtr preparsedData,
			ref HIDP_CAPS caps);

		[DllImport("Hid.dll", SetLastError = false, CharSet = CharSet.Auto)]
		internal static extern uint HidP_GetButtonCaps(
			int inputReportType,   // HidP_Input, HidP_Output, HidP_Feature
			ref HIDP_BUTTON_CAPS buttonCaps,
			ref ushort buttonCapsLength,
			IntPtr preparsedData);

		// HIDP_REPORT_TYPE enumeration
		public const int HidP_Input = 0;
		public const int HidP_Output = 1;
		public const int HidP_Feature = 2;
	}
}

