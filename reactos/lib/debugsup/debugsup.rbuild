<?xml version="1.0"?>
<!DOCTYPE group SYSTEM "../../tools/rbuild/project.dtd">
<group>
	<module name="debugsup_ntoskrnl" type="staticlibrary">
		<importlibrary definition="debugsup-ntos.def" dllname="ntoskrnl.exe" />
	</module>
	<module name="debugsup_ntdll" type="staticlibrary">
		<importlibrary definition="debugsup-ntos.def" dllname="ntdll.dll" />
	</module>
</group>
