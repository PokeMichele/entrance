#%PAM-1.0

auth		include		system-login
-auth		optional	pam_gnome_keyring.so
-auth		optional	pam_kwallet6.so
account		include		system-login
password	include		system-login
session		include		system-login
-session	optional	pam_gnome_keyring.so auto_start
-session	optional	pam_kwallet6.so auto_start

