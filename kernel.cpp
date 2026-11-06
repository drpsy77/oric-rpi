//
// kernel.cpp — Kernel Circle de l'émulateur Oric (port de Phosphoric)
//
// Code d'intégration propre au port. Les motifs Circle sont repris de
// (GPL-3.0, voir SOURCES.md) :
//   - circle-stdlib samples/03-stdio-fatfs  : classe CStdlibAppStdio, SD + stdio
//   - Circle sample/08-usbkeyboard          : clavier USB en mode brut
//   - Circle sample/34-sounddevices         : son HDMI via file d'attente Write()
// La table clavier est la table QWERTY de Phosphoric (src/io/keyboard.c,
// elle-même reprise d'Oricutron), transposée des codes SDL en codes HID USB.
//
// Options dans cmdline.txt (carte SD) :
//   width=590 height=448          taille du framebuffer (le GPU agrandit) :
//                                 590x448 pour pixels=tele, 240x224 pour carres
//   pixels=tele|carres            forme des points (défaut tele, 1,23:1)
//   couleurs=tele|pures           couleurs du téléviseur ou 00/FF (défaut tele)
//   saturation=100 luminosite=100 couleurs plus vives / plus claires (en %)
//   rouge=FF2A1A ...              une couleur imposée (RRGGBB) : noir, rouge,
//                                 vert, jaune, bleu, magenta, cyan, blanc
//   keymap=FR                     clavier AZERTY (lettres A/Q, Z/W, M, ; ,)
//   rom=oric/basic11b.rom         ROM système (défaut)
//   diskrom=oric/microdis.rom     ROM du Microdisc  } les deux pour activer
//   disk=oric/xxx.dsk             disquette lecteur A } le Microdisc
//   tape=oric/xxx.tap             cassette, lue par CLOAD""
//   fdctiming=fast                Microdisc sans délais mécaniques
//                                 (défaut real = vitesse du vrai lecteur)
//   loci=1                        cartouche LOCI ; ROM par défaut : son menu
//   lociflash=oric/loci           mémoire flash interne « 0: » (défaut)
//   lociusb=oric/usb              clé USB « 1: » (défaut)
//   modem=1                       modem Wi-Fi de LOCI (picowifi de Phosphoric,
//                                 ACIA en $0380) ; firmware/ et
//                                 wpa_supplicant.conf à la racine de la carte
//   modemcadence=1                ACIA du modem cadencée sur son débit (9600
//                                 bauds) ; défaut 0 : sans cadence, comme LOCI
//   locidisk=0:/cpa-en.dsk        avec loci=1 : image montée en A: de LOCI et
//                                 démarrage direct dessus (sans le menu)
//   ntp=pool.ntp.org              avec modem=1 : serveur de l'heure (dates
//                                 des fichiers écrits sur la carte SD)
//   fuseau=CET-1CEST,M3.5.0,M10.5.0/3  fuseau horaire, règle POSIX de TZ
//                                 (défaut : Paris, heure d'été comprise)
// Ces réglages (sauf width, height, lociflash) peuvent aussi être mis dans
// oric.cfg du dossier lociflash (0:ORIC.CFG pour l'Oric), qui l'emporte.
// Journal de Phosphoric : oric/journal.txt (session précédente :
// oric/journal-precedent.txt). Émulation sur le cœur 1, le reste sur le cœur 0.
// Touches : F5 = reset, Maj+F5 = NMI, F8 = bouton Action de LOCI (comme
//           Phosphoric), F12 = ajoute une mesure de performance à oric/perf.txt,
//           Ctrl+Alt+Suppr = disquettes réécrites, puis redémarrage de la Pi,
//           Ctrl+Alt+Fin = idem, puis arrêt : la Pi peut être débranchée
//           (message à l'écran, LED allumée fixe).
//
#include "kernel.h"
#include "oric_machine.h"
#include "journal.h"
#include "netrelay.h"

#include <circle/machineinfo.h>
#include <circle/memory.h>
#include <circle/synchronize.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/timer.h>
#include <circle/usb/usbhid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_US        ORIC_FRAME_US               // une trame PAL Oric = 20 ms
#define MAX_SAMPLES     1024                        // > 882 (44,1 kHz / 50)
#define SOUND_QUEUE_MS  100
#define CHUNK_WORDS     (384 * 10)                  // bloc du son HDMI : 1920 échantillons stéréo
#define SOUND_TARGET    (3 * 882)                   // réserve moyenne visée : ~60 ms
#define MAX_CORRECTION_US 200                       // ±1 % au plus
#define STATS_FRAMES    500                         // mesure toutes les 10 s

static const char FromOric[] = "oric";

static int16_t s_SoundBuf[MAX_SAMPLES * 2];         // stéréo entrelacé
static unsigned s_nSamplesFrame;                    // échantillons par trame

CKernel *CKernel::s_pThis = 0;

CEmuCores::CEmuCores (CKernel *pKernel)
:	CMultiCoreSupport (CMemorySystem::Get ()),
	m_pKernel (pKernel)
{
}

void CEmuCores::Run (unsigned nCore)
{
	if (nCore == 1)
	{
		m_pKernel->RunEmulation ();
	}
}

CKernel::CKernel (void)
:	CStdlibAppNetwork (FromOric, CSTDLIBAPP_DEFAULT_PARTITION, 0, 0, 0, 0, NetDeviceTypeWLAN),
	m_CPUThrottle (CPUSpeedMaximum),
	m_EmuCores (this),
	m_PanicScreen (&mScreen),
	m_bModem (false),
	m_bNetStarting (false),
	m_pNTPDaemon (0),
	m_nClockCheck (0),
	m_bClockSet (false),
	m_pKeyboard (0),
	m_pSound (0),
	m_bAzerty (false),
	m_nScaleY (1),
	m_nOutWidth (0), m_nOutHeight (0),
	m_nOffsetX (0), m_nOffsetY (0),
	m_bResetRequest (false),
	m_bNMIRequest (false),
	m_bPerfRequest (false),
	m_bRebootRequest (false),
	m_bHaltRequest (false),
	m_bLociDown (false),
	m_bLociUp (false),
	m_bHIDChanged (false),
	m_ucHIDModifiers (0),
	m_bF8Held (false),
	m_bMachineReady (false),
	m_nFramesRequested (0),
	m_nFramesDone (0),
	m_nLastAvg (0), m_nLastMax (0), m_nLastSkipped (0),
	m_nWorst (0), m_nWindows (0),
	m_nPerfCount (0)
{
	s_pThis = this;
	mActLED.Blink (5);	// show we are alive
}

// Comme CStdlibAppNetwork::InitializeNoWaitForActivate, mais le Wi-Fi n'est
// démarré qu'avec modem=1, et son échec n'empêche pas l'Oric de démarrer.
// Le support multicœur se démarre en dernier (doc/multicore.txt de Circle) ;
// le cœur 1 attend ensuite que Run() ait préparé la machine.
bool CKernel::Initialize (void)
{
	if (!CStdlibAppStdio::Initialize ())
	{
		return false;
	}

	ReadSettings ();		// carte SD montée : oric.cfg

	m_bModem = OptionDecimal ("modem", 0) == 1;
	if (m_bModem)
	{
		m_bNetStarting =    mWLAN.Initialize ()
				 && mNet.Initialize (FALSE)
				 && mWPASupplicant.Initialize ();
		if (!m_bNetStarting)
		{
			mLogger.Write (FromOric, LogWarning,
				       "Wi-Fi indisponible (firmware/ et wpa_supplicant.conf sur la carte SD ?)");
		}
	}

	return m_EmuCores.Initialize ();
}

// Réglages de oric.cfg, dans la mémoire flash de LOCI (lociflash=, vue par
// l'Oric comme 0:ORIC.CFG) : modifiable depuis CP/A, alors que cmdline.txt,
// à la racine de la carte, est hors de portée de LOCI. Mêmes noms que dans
// cmdline.txt, « nom=valeur » séparés par des espaces ou des fins de ligne,
// « # » jusqu'à la fin de la ligne = commentaire. Ils remplacent ceux de
// cmdline.txt, sauf width, height et lociflash (lus avant).
#define MAX_SETTINGS	32
static char s_SettingsText[4096];
static const char *s_pSettingName[MAX_SETTINGS];
static const char *s_pSettingValue[MAX_SETTINGS];
static unsigned s_nSettings;
static bool s_bSettingsFile;

void CKernel::ReadSettings (void)
{
	char Path[128];
	snprintf (Path, sizeof Path, "%s/oric.cfg",
		  mOptions.GetAppOptionString ("lociflash", "oric/loci"));
	FILE *pFile = fopen (Path, "r");
	if (pFile == 0)
	{
		return;
	}
	size_t nLength = fread (s_SettingsText, 1, sizeof s_SettingsText - 1, pFile);
	fclose (pFile);
	s_SettingsText[nLength] = '\0';
	s_bSettingsFile = true;

	char *pLine = s_SettingsText;
	while (pLine != 0)
	{
		char *pNext = strchr (pLine, '\n');
		if (pNext != 0)
		{
			*pNext++ = '\0';
		}
		char *pComment = strchr (pLine, '#');
		if (pComment != 0)
		{
			*pComment = '\0';
		}

		char *pSave;
		for (char *pWord = strtok_r (pLine, " \t\r", &pSave); pWord != 0;
		     pWord = strtok_r (0, " \t\r", &pSave))
		{
			char *pEqual = strchr (pWord, '=');
			if (pEqual != 0 && pEqual != pWord && s_nSettings < MAX_SETTINGS)
			{
				*pEqual = '\0';			// mot sans « = » : ignoré
				s_pSettingName[s_nSettings] = pWord;
				s_pSettingValue[s_nSettings] = pEqual + 1;
				s_nSettings++;
			}
		}

		pLine = pNext;
	}
}

const char *CKernel::Option (const char *pName, const char *pDefault)
{
	for (unsigned i = 0; i < s_nSettings; i++)
	{
		if (strcmp (s_pSettingName[i], pName) == 0)
		{
			return s_pSettingValue[i];
		}
	}
	return mOptions.GetAppOptionString (pName, pDefault);
}

unsigned CKernel::OptionDecimal (const char *pName, unsigned nDefault)
{
	for (unsigned i = 0; i < s_nSettings; i++)
	{
		if (strcmp (s_pSettingName[i], pName) == 0)
		{
			char *pEnd;
			unsigned long nValue = strtoul (s_pSettingValue[i], &pEnd, 10);
			return *pEnd == '\0' && pEnd != s_pSettingValue[i] ? nValue : nDefault;
		}
	}
	return mOptions.GetAppOptionDecimal (pName, nDefault);
}

// Ctrl+Alt+Fin : écran noir et message (police de Circle, 8x16), LED fixe.
// La carte est démontée juste après (main.cpp), puis la Pi s'arrête.
void CKernel::ShowHalted (void)
{
	CBcmFrameBuffer *pFrameBuffer = mScreen.GetFrameBuffer ();
	u16 *pDest = (u16 *) (uintptr) pFrameBuffer->GetBuffer ();
	unsigned nPitch = pFrameBuffer->GetPitch () / sizeof (u16);
	for (unsigned y = 0; y < pFrameBuffer->GetHeight (); y++)
	{
		memset (pDest + y * nPitch, 0, pFrameBuffer->GetWidth () * sizeof (u16));
	}

	static const char Message[] = "Oric arrete : vous pouvez debrancher.";
	char Text[80];
	snprintf (Text, sizeof Text, "\x1b[%u;%uH%s",
		  mScreen.GetRows () / 2,
		  (mScreen.GetColumns () - (sizeof Message - 1)) / 2 + 1, Message);
	mScreen.Write (Text, strlen (Text));
	mActLED.On ();
}

// Messages de Circle (Wi-Fi, USB...) : de la mémoire du logger au journal.
void CKernel::LogToJournal (void)
{
	char Text[200];
	int nLength;
	while ((nLength = mLogger.Read (Text, sizeof Text - 1)) > 0)
	{
		Text[nLength] = '\0';
		JournalPrintf ("%s", Text);
	}
}

int CPanicScreen::Write (const void *pBuffer, size_t nCount)
{
	if (nCount >= 4 && memcmp (pBuffer, "\x1b[1m", 4) == 0)
	{
		return m_pScreen->Write (pBuffer, nCount);	// panique : visible
	}
	return nCount;
}

// Cœur 0 : le réseau est prêt dès que le Wi-Fi a une adresse (DHCP).
void CKernel::UpdateNetwork (void)
{
	if (!m_bNetStarting || !mNet.IsRunning ())
	{
		return;
	}
	m_bNetStarting = false;

	CGlueNetworkInit (mNet);
	NetRelaySetReady ();

	CString IPAddress;
	mNet.GetConfig ()->GetIPAddress ()->Format (&IPAddress);
	JournalPrintf ("Wi-Fi : connecte, adresse %s\n", (const char *) IPAddress);

	// Heure par NTP : une tâche du cœur 0, nouvel essai toutes les 5 min
	// tant que le serveur ne répond pas, puis toutes les 15 min
	m_pNTPDaemon = new CNTPDaemon (Option ("ntp", "pool.ntp.org"), &mNet);
}

// Cœur 0, à chaque tour : une fois l'heure reçue (NTP, en temps universel),
// décalage du fuseau (fuseau=, heure d'été comprise) recalculé chaque
// seconde et appliqué à l'horloge de Circle, dont la FatFs date les
// fichiers. circle-newlib (time, localtime de Phosphoric) suit TZ.
void CKernel::UpdateClock (void)
{
	unsigned nNow = CTimer::GetClockTicks ();
	if (nNow - m_nClockCheck < 1000000)
	{
		return;
	}
	m_nClockCheck = nNow;

	CTimer *pTimer = CTimer::Get ();
	time_t nUTC = pTimer->GetUniversalTime ();
	if (nUTC < 1600000000)		// pas encore d'heure (avant 2020)
	{
		return;
	}

	struct tm Local;
	if (localtime_r (&nUTC, &Local) == 0)
	{
		return;
	}
	// Heure locale en secondes depuis 1970 (jours depuis le 1er janvier 1970
	// du calendrier civil : H. Hinnant, « days_from_civil »)
	int y = Local.tm_year + 1900 - (Local.tm_mon < 2);
	int era = y / 400;
	int yoe = y - era * 400;
	int doy = (153 * (Local.tm_mon + (Local.tm_mon < 2 ? 10 : -2)) + 2) / 5 + Local.tm_mday - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	long long nLocal = (long long) (era * 146097 + doe - 719468) * 86400
			   + Local.tm_hour * 3600 + Local.tm_min * 60 + Local.tm_sec;
	int nMinutes = (int) ((nLocal - nUTC) / 60);

	if (nMinutes != pTimer->GetTimeZone () || !m_bClockSet)
	{
		// L'horloge de Circle garde l'heure locale : la reposer avec le fuseau
		pTimer->SetTimeZone (nMinutes);
		pTimer->SetTime ((unsigned) nUTC, FALSE);
		m_bClockSet = true;
		JournalPrintf ("Heure : %04d-%02d-%02d %02d:%02d:%02d (UTC%+d min)\n",
			       Local.tm_year + 1900, Local.tm_mon + 1, Local.tm_mday,
			       Local.tm_hour, Local.tm_min, Local.tm_sec, nMinutes);
	}
}

// Cœur 0 : préparation, puis clavier USB, son, température, carte SD.
CStdlibApp::TShutdownMode CKernel::Run (void)
{
	mLogger.Write (FromOric, LogNotice, "Phosphoric %s sur Circle", oric_machine_version ());

	const char *pKeyMap = Option ("keymap", mOptions.GetKeyMap ());
	m_bAzerty = pKeyMap != 0 && strcmp (pKeyMap, "FR") == 0;

	// Journal de Phosphoric dans oric/journal.txt (plus rien sur l'image)
	FILE *pJournal = JournalOpen ("oric/journal.txt", "oric/journal-precedent.txt");
	if (pJournal != 0)
	{
		oric_machine_log_to (pJournal);
	}

	if (s_bSettingsFile)
	{
		JournalPrintf ("oric.cfg : %u reglage(s)\n", s_nSettings);
		for (unsigned i = 0; i < s_nSettings; i++)
		{
			JournalPrintf ("  %s=%s\n", s_pSettingName[i], s_pSettingValue[i]);
		}
	}

	// Fuseau horaire (règle POSIX de TZ), avant que le cœur 1 ne démarre
	setenv ("TZ", Option ("fuseau", "CET-1CEST,M3.5.0,M10.5.0/3"), 1);
	tzset ();

	if (m_bModem && !m_bNetStarting)
	{
		JournalPrintf ("Wi-Fi : demarrage impossible (firmware/, wpa_supplicant.conf ?)\n");
	}

	oric_media_t Media;
	memset (&Media, 0, sizeof Media);
	if (m_bModem)
	{
		// Modem Wi-Fi de LOCI, ACIA en $0380. Le picowifi de Phosphoric simule
		// l'association au Wi-Fi dès qu'il a un nom de réseau ; la vraie
		// liaison est celle de la Pi (wpa_supplicant.conf).
		Media.serial = "picowifi:oric-rpi";
		// Réglages du modem gardés par AT&W (numéros abrégés...) : dans la
		// mémoire flash de LOCI, comme sur le vrai Pico W
		setenv ("PHOSPHORIC_PICOWIFI_NVRAM", "oric/loci/picowifi.cfg", 1);
		// ACIA sans cadence (comme le vrai LOCI), sauf modemcadence=1
		Media.serial_cadence = OptionDecimal ("modemcadence", 0) == 1;
	}
	Media.loci = OptionDecimal ("loci", 0) == 1;
	Media.loci_flash_dir = mOptions.GetAppOptionString ("lociflash", "oric/loci");
	Media.loci_usb_dir   = Option ("lociusb", "oric/usb");
	Media.loci_disk      = Option ("locidisk");	// ex. 0:/cpa.dsk : démarrage direct

	// ROM système : rom= ; à défaut, le menu LOCI (si LOCI) ou le BASIC 1.1
	static char LociMenuROM[128];
	snprintf (LociMenuROM, sizeof LociMenuROM, "%s/locirom", Media.loci_flash_dir);
	Media.rom_file = Option ("rom",
				Media.loci ? LociMenuROM : "oric/basic11b.rom");
	Media.tape_file     = Option ("tape");
	Media.fdc_timing    = Option ("fdctiming");	// fast | real
	const char *pDisk    = Option ("disk");
	const char *pDiskROM = Option ("diskrom");
	if (pDisk != 0 && pDiskROM != 0)	// Microdisc seulement avec ROM + disquette
	{
		Media.disk_rom_file = pDiskROM;
		Media.disk_files[0] = pDisk;
	}

	if (!oric_machine_init (&Media))
	{
		JournalFlush ();
		mLogger.Write (FromOric, LogPanic, "Initialisation de l'Oric impossible (voir oric/journal.txt)");
	}
	SetupDisplay ();
	mLogger.SetNewTarget (&m_PanicScreen);	// plus de messages sur l'image
	LogToJournal ();
	JournalFlush ();

	s_nSamplesFrame = oric_machine_audio_rate () * FRAME_US / 1000000;	// 882

	StartSound ();

	DataMemBarrier ();
	m_bMachineReady = true;			// le cœur 1 peut démarrer

	// Cadence : une trame toutes les 20 ms du timer, régulière. Le son HDMI
	// prend sa file par blocs de ~43 ms (CHUNK_WORDS) : cadencer sur sa file
	// donnait des trames par paires suivies d'un trou (saccades). La file ne
	// sert plus qu'à corriger lentement l'écart entre l'horloge du son et le
	// timer (nCorrection, quelques µs par trame), sur sa moyenne de 1 s.
	unsigned nWritten = 0;			// trames dont le son est parti
	unsigned nDeadline = CTimer::GetClockTicks ();
	unsigned nLastFlush = nDeadline;
	bool bPrimed = m_pSound == 0;		// file du son remplie au démarrage
	unsigned nShortWrites = 0;		// écritures refusées d'affilée (file pleine)
	unsigned nLevelSum = 0, nLevelCount = 0;
	int nCorrection = 0;			// µs ajoutées à chaque trame

	while (true)
	{
		mScheduler.Yield ();		// pile réseau, Wi-Fi
		NetRelayService ();		// sockets et fermetures demandés par le cœur 1
		UpdateNetwork ();
		UpdateClock ();
		UpdateKeyboard ();
		m_CPUThrottle.Update ();	// surveille la température (toutes les 2 à 5 s)

		if (m_nFramesDone != m_nFramesRequested)
		{
			continue;		// le cœur 1 calcule la trame
		}
		DataMemBarrier ();

		// Trame faite : le cœur 1 attend, on peut toucher au son et aux fichiers
		if (nWritten != m_nFramesDone)
		{
			nWritten = m_nFramesDone;
			if (m_pSound != 0)
			{
				int nBytes = s_nSamplesFrame * 2 * sizeof (int16_t);
				nShortWrites = m_pSound->Write (s_SoundBuf, nBytes) < nBytes ? nShortWrites + 1 : 0;
				if (nShortWrites > 10)
				{
					// le son ne se vide plus (pas de récepteur HDMI audio ?)
					mLogger.Write (FromOric, LogWarning, "Son HDMI bloque, cadence sur le timer");
					m_pSound->Cancel ();
					m_pSound = 0;
					bPrimed = true;
					nCorrection = 0;
				}
				else if (bPrimed)
				{
					nLevelSum += m_pSound->GetQueueFramesAvail ();
					if (++nLevelCount == 50)
					{
						// excès moyen de la file, résorbé en ~5 s (250 trames)
						int nExcess = (int) (nLevelSum / 50) - (int) SOUND_TARGET;
						nCorrection = (int) (nExcess * 1000000LL / oric_machine_audio_rate () / 250);
						if (nCorrection > MAX_CORRECTION_US)
						{
							nCorrection = MAX_CORRECTION_US;
						}
						else if (nCorrection < -MAX_CORRECTION_US)
						{
							nCorrection = -MAX_CORRECTION_US;
						}
						nLevelSum = nLevelCount = 0;
					}
				}
			}
		}

		if (m_bRebootRequest || m_bHaltRequest)
		{
			// Ctrl+Alt+Suppr, Ctrl+Alt+Fin : le cœur 1 attend la trame
			// suivante, qui ne viendra plus. Disquettes, fichiers de LOCI et
			// journal sur la carte ; main.cpp démonte ensuite la carte
			// (Cleanup : f_mount) puis redémarre ou arrête la Pi.
			if (m_pSound != 0)
			{
				m_pSound->Cancel ();
			}
			oric_machine_shutdown ();
			JournalPrintf (m_bHaltRequest ? "Arret (Ctrl+Alt+Fin)\n"
						      : "Redemarrage (Ctrl+Alt+Suppr)\n");
			if (pJournal != 0)
			{
				fflush (pJournal);
			}
			LogToJournal ();
			JournalFlush ();
			if (!m_bHaltRequest)
			{
				return ShutdownReboot;
			}
			ShowHalted ();
			return ShutdownHalt;
		}

		if (m_bPerfRequest)
		{
			m_bPerfRequest = false;
			LogToJournal ();
			JournalFlush ();
			WritePerf ();
		}
		if (CTimer::GetClockTicks () - nLastFlush > 2000000)	// toutes les 2 s
		{
			LogToJournal ();
			JournalFlush ();
			nLastFlush = CTimer::GetClockTicks ();
		}

		if (!bPrimed)
		{
			// Démarrage : trames sans attendre jusqu'à la réserve de son
			if (m_pSound->GetQueueFramesAvail () >= SOUND_TARGET)
			{
				bPrimed = true;
				nDeadline = CTimer::GetClockTicks ();
			}
		}
		else
		{
			if ((int) (CTimer::GetClockTicks () - nDeadline) < 0)
			{
				continue;
			}
			nDeadline += FRAME_US + nCorrection;
			if ((int) (CTimer::GetClockTicks () - nDeadline) > 5 * FRAME_US)
			{
				nDeadline = CTimer::GetClockTicks ();	// trop en retard : on recale
			}
		}

		DataMemBarrier ();
		m_nFramesRequested = m_nFramesDone + 1;		// trame suivante
	}

	return ShutdownHalt;
}

// Demandes du clavier, prises en compte entre deux trames (cœur 1)
void CKernel::ApplyRequests (void)
{
	if (m_bResetRequest)
	{
		m_bResetRequest = false;
		oric_machine_reset ();
	}
	if (m_bNMIRequest)
	{
		m_bNMIRequest = false;
		oric_machine_nmi ();
	}
	if (m_bLociDown)
	{
		m_bLociDown = false;
		oric_machine_loci_button (true);
	}
	if (m_bLociUp)
	{
		m_bLociUp = false;
		oric_machine_loci_button (false);
	}
	if (m_bHIDChanged)
	{
		m_bHIDChanged = false;
		unsigned char Keys[6];
		for (unsigned i = 0; i < 6; i++)
		{
			Keys[i] = m_HIDKeys[i];
		}
		oric_machine_loci_kbd_report (m_ucHIDModifiers, Keys);
	}
}

// Cœur 1 : une trame (image + son) à chaque demande du cœur 0.
void CKernel::RunEmulation (void)
{
	while (!m_bMachineReady)
	{
		// attente de la préparation par le cœur 0
	}
	DataMemBarrier ();

	unsigned nFrames = 0, nEmuSum = 0, nEmuMax = 0, nSkipped = 0;
	bool bSkipBlit = false;

	while (true)
	{
		while (m_nFramesRequested == m_nFramesDone)
		{
			// attente active de la demande suivante
		}
		DataMemBarrier ();

		ApplyRequests ();

		// Émulation d'une trame + son correspondant
		unsigned nStart = CTimer::GetClockTicks ();
		oric_machine_run_frame ();
		oric_machine_audio (s_SoundBuf, s_nSamplesFrame);
		unsigned nEmu = CTimer::GetClockTicks () - nStart;

		// Affichage, sauté si la trame précédente a dépassé son budget
		if (!bSkipBlit)
		{
			Blit ();
		}
		else
		{
			nSkipped++;
		}
		bSkipBlit = nEmu > FRAME_US;

		// Mesure de performance, gardée en mémoire (rien à l'écran :
		// le journal s'afficherait par-dessus l'image)
		nEmuSum += nEmu;
		if (nEmu > nEmuMax)
		{
			nEmuMax = nEmu;
		}
		if (++nFrames == STATS_FRAMES)
		{
			m_nLastAvg = nEmuSum / STATS_FRAMES;
			m_nLastMax = nEmuMax;
			m_nLastSkipped = nSkipped;
			if (nEmuMax > m_nWorst)
			{
				m_nWorst = nEmuMax;
			}
			m_nWindows++;
			nFrames = nEmuSum = nEmuMax = nSkipped = 0;
		}

		DataMemBarrier ();
		m_nFramesDone = m_nFramesDone + 1;
	}
}

// Repris de Circle sample/34-sounddevices (cas "sndhdmi") : le Pi Zero
// n'a pas de sortie jack, le son passe par le HDMI.
void CKernel::StartSound (void)
{
	m_pSound = new CHDMISoundBaseDevice (&mInterrupt, oric_machine_audio_rate (), CHUNK_WORDS);

	if (   !m_pSound->AllocateQueue (SOUND_QUEUE_MS)
	    || (m_pSound->SetWriteFormat (SoundFormatSigned16, 2), !m_pSound->Start ()))
	{
		mLogger.Write (FromOric, LogWarning, "Son HDMI indisponible, cadence sur le timer");
		delete m_pSound;
		m_pSound = 0;
	}
}

// Repris de Circle sample/08-usbkeyboard (branchement à chaud du clavier)
void CKernel::UpdateKeyboard (void)
{
	boolean bUpdated = mUSBHCI.UpdatePlugAndPlay ();

	if (bUpdated && m_pKeyboard == 0)
	{
		m_pKeyboard = (CUSBKeyboardDevice *) mDeviceNameService.GetDevice ("ukbd1", FALSE);
		if (m_pKeyboard != 0)
		{
			m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler);
			m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
			mLogger.Write (FromOric, LogNotice, "Clavier USB connecte");
		}
	}
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pKeyboard = 0;
	oric_machine_keyboard_release_all ();
}

// ── Clavier ──────────────────────────────────────────────────────────────
// Table QWERTY de Phosphoric src/io/keyboard.c (qwerty_tab, d'après
// Oricutron), même ordre : index = colonne * 8 + ligne, codes SDL remplacés
// par les codes d'usage HID USB de la même touche. 0 = pas de touche ;
// les modificateurs (Ctrl, Shift, Alt) sont traités à part.
static const unsigned char s_OricKeys[64] =
{
	/* Col 0 */ 0x24, 0x11, 0x22, 0x19,  0,    0x1E, 0x1B, 0x20,	// 7 n 5 v RCTRL 1 x 3
	/* Col 1 */ 0x0D, 0x17, 0x15, 0x09,  0,    0x29, 0x14, 0x07,	// j t r f - ESC q d
	/* Col 2 */ 0x10, 0x23, 0x05, 0x21,  0,    0x1D, 0x1F, 0x06,	// m 6 b 4 LCTRL z 2 c
	/* Col 3 */ 0x0E, 0x26, 0x33, 0x2D,  0,    0,    0x31, 0x34,	// k 9 ; - - - \ '
	/* Col 4 */ 0x2C, 0x36, 0x37, 0x52,  0,    0x50, 0x51, 0x4F,	// SPC , . UP LSHIFT LEFT DOWN RIGHT
	/* Col 5 */ 0x18, 0x0C, 0x12, 0x13,  0,    0x2A, 0x30, 0x2F,	// u i o p LALT BKSP ] [
	/* Col 6 */ 0x1C, 0x0B, 0x0A, 0x08,  0,    0x04, 0x16, 0x1A,	// y h g e RALT a s w
	/* Col 7 */ 0x25, 0x0F, 0x27, 0x38,  0,    0x28, 0x35, 0x2E	// 8 l 0 / RSHIFT RETURN ` =
};

// Modificateurs HID -> position Oric (ligne 4 de la table de Phosphoric)
static const struct { unsigned char ucMask; int nCol, nRow; } s_OricMods[] =
{
	{ RCTRL,  0, 4 }, { LCTRL,  2, 4 }, { LSHIFT, 4, 4 },
	{ ALT,    5, 4 }, { ALTGR,  6, 4 }, { RSHIFT, 7, 4 }	// Alt = FUNCT
};

// Clavier AZERTY (keymap=FR) : ramène les lettres déplacées à leur code QWERTY.
static unsigned char AzertyToQwerty (unsigned char ucKey)
{
	switch (ucKey)
	{
	case 0x14: return 0x04;	// touche A  -> a
	case 0x04: return 0x14;	// touche Q  -> q
	case 0x1A: return 0x1D;	// touche Z  -> z
	case 0x1D: return 0x1A;	// touche W  -> w
	case 0x33: return 0x10;	// touche M  -> m
	case 0x10: return 0x36;	// touche ,  -> ,
	case 0x36: return 0x33;	// touche ;  -> ;
	default:   return ucKey;
	}
}

// Appelé en interruption à chaque rapport du clavier : on reconstruit toute
// la matrice (actif bas, comme oric_keyboard_t.matrix de Phosphoric).
void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	// Rapport brut transmis tel quel à LOCI (appliqué dans la boucle principale)
	s_pThis->m_ucHIDModifiers = ucModifiers;
	for (unsigned i = 0; i < 6; i++)
	{
		s_pThis->m_HIDKeys[i] = RawKeys[i];
	}
	s_pThis->m_bHIDChanged = true;

	// F8 = bouton Action de LOCI : appui et relâchement
	bool bF8 = false;
	for (unsigned i = 0; i < 6; i++)
	{
		if (RawKeys[i] == 0x41)
		{
			bF8 = true;
		}
	}
	if (bF8 && !s_pThis->m_bF8Held)
	{
		s_pThis->m_bLociDown = true;
	}
	else if (!bF8 && s_pThis->m_bF8Held)
	{
		s_pThis->m_bLociUp = true;
	}
	s_pThis->m_bF8Held = bF8;

	unsigned char Matrix[8];
	memset (Matrix, 0xFF, sizeof Matrix);

	for (unsigned i = 0; i < sizeof s_OricMods / sizeof s_OricMods[0]; i++)
	{
		if (ucModifiers & s_OricMods[i].ucMask)
		{
			Matrix[s_OricMods[i].nCol] &= ~(1 << s_OricMods[i].nRow);
		}
	}

	for (unsigned i = 0; i < 6; i++)
	{
		unsigned char ucKey = RawKeys[i];
		if (ucKey == 0)
		{
			continue;
		}

		if (ucKey == 0x3E)			// F5 : reset, Maj+F5 : NMI
		{
			if (ucModifiers & (LSHIFT | RSHIFT))
			{
				s_pThis->m_bNMIRequest = true;
			}
			else
			{
				s_pThis->m_bResetRequest = true;
			}
			continue;
		}
		if (ucKey == 0x45)			// F12 : mesure de performance
		{
			s_pThis->m_bPerfRequest = true;
			continue;
		}
		if (   (ucKey == 0x4C || ucKey == 0x4D)	// Ctrl+Alt+Suppr : redémarrage
		    && (ucModifiers & (LCTRL | RCTRL))	// Ctrl+Alt+Fin : arrêt
		    && (ucModifiers & (ALT | ALTGR)))
		{
			if (ucKey == 0x4C)
			{
				s_pThis->m_bRebootRequest = true;
			}
			else
			{
				s_pThis->m_bHaltRequest = true;
			}
			continue;
		}
		if (ucKey == 0x4C)			// Suppr = BKSP (comme Phosphoric)
		{
			ucKey = 0x2A;
		}
		if (s_pThis->m_bAzerty)
		{
			ucKey = AzertyToQwerty (ucKey);
		}

		for (unsigned j = 0; j < 64; j++)
		{
			if (s_OricKeys[j] == ucKey)
			{
				Matrix[j / 8] &= ~(1 << (j % 8));
				break;
			}
		}
	}

	memcpy (oric_machine_keyboard_matrix (), Matrix, sizeof Matrix);
}

// ── Mesure de performance ─────────────────────────────────────────────────
// Ajoutée sur demande (F12) à la fin de oric/perf.txt, pour être lue sur un
// autre ordinateur : chaque appui donne une mesure numérotée, avec le temps
// écoulé depuis le démarrage. L'écriture sur la carte SD peut provoquer un
// petit à-coup.
void CKernel::WritePerf (void)
{
	FILE *pFile = fopen ("oric/perf.txt", "a");
	if (pFile == 0)
	{
		return;
	}

	unsigned nUptime = CTimer::Get ()->GetUptime ();	// secondes
	unsigned nARM = CMachineInfo::Get ()->GetClockRate (CLOCK_ID_ARM);

	fprintf (pFile, "=== mesure %u, %u min %02u s apres le demarrage ===\n",
		 ++m_nPerfCount, nUptime / 60, nUptime % 60);
	fprintf (pFile, "Phosphoric %s sur Circle - temps d'emulation d'une trame\n",
		 oric_machine_version ());
	fprintf (pFile, "processeur ARM         : %u MHz (maximum %u MHz), %u C\n",
		 nARM / 1000000, m_CPUThrottle.GetMaxClockRate () / 1000000,
		 m_CPUThrottle.GetTemperature ());
	fprintf (pFile, "budget par trame       : %u us (50 Hz)\n", FRAME_US);
	if (m_nWindows == 0)
	{
		fprintf (pFile, "pas encore de mesure : attendre 10 s apres le demarrage\n");
	}
	else
	{
		fprintf (pFile, "dernieres 10 s        : moyenne %u us, max %u us, %u images sautees\n",
			 m_nLastAvg, m_nLastMax, m_nLastSkipped);
		fprintf (pFile, "pire trame depuis le demarrage : %u us (%u mesures de 10 s)\n",
			 m_nWorst, m_nWindows);
		fprintf (pFile, "charge moyenne        : %u %%\n", m_nLastAvg * 100 / FRAME_US);
	}
	fprintf (pFile, "son HDMI              : %s\n\n", m_pSound != 0 ? "actif" : "inactif");
	fclose (pFile);
}

// ── Affichage ────────────────────────────────────────────────────────────
// L'image RGB888 de Phosphoric (240x224) est copiée dans le framebuffer
// 16 bits de Circle (taille : width= height= de cmdline.txt), puis le GPU
// l'agrandit à l'écran en gardant les proportions (avec un léger flou).
//   pixels=tele   : points 1,23 fois plus larges que hauts, comme sur un
//                   téléviseur PAL (point de l'Oric à 6 MHz, points carrés
//                   à 14,75 MHz : 14,75 / 6 / 2 trames) ; width=590 height=448
//   pixels=carres : points carrés (comme Oricutron) ; width=240 height=224
//   couleurs=tele : les 6 couleurs comme sur le téléviseur de Pierre (réglage
//                   de cpa-oric, docs/console-distance.md § 9) ; =pures : 00/FF
//   saturation=, luminosite= (en %, défaut 100), puis rouge=RRGGBB... : la
//                   palette des 8 couleurs, calculée une fois
// En hauteur, chaque ligne est répétée un nombre entier de fois ; en largeur,
// chaque point de l'écran prend la part des points de l'Oric qu'il couvre :
// au plus un point adouci par bord, comme la page web de cpa-oric.

#define TELE_RATIO_X100	123
#define MAX_SRC_WIDTH	512
#define MAX_OUT_WIDTH	4096

// Couleurs Tele de cpa-oric (tools/remote.html), dans l'ordre de l'Oric
// (bit 0 rouge, bit 1 vert, bit 2 bleu)
static const u8 s_TeleRGB[8][3] =
{
	{0x00, 0x00, 0x00}, {0xFF, 0x2A, 0x1A}, {0x2E, 0xC2, 0x2E}, {0xFD, 0xDE, 0x53},
	{0x0A, 0x3C, 0xFF}, {0xEE, 0x22, 0xEE}, {0x18, 0xE8, 0xE8}, {0xFF, 0xFF, 0xFF}
};

// Palette affichée (SetupDisplay), et noms des couleurs dans oric.cfg
static u8 s_Palette[8][3];
static const char *const s_ColorName[8] =
{
	"noir", "rouge", "vert", "jaune", "bleu", "magenta", "cyan", "blanc"
};

static u8 Clamp255 (int nValue)
{
	return nValue < 0 ? 0 : nValue > 255 ? 255 : nValue;
}

static u16 s_SrcCol[MAX_OUT_WIDTH];	// point de l'Oric à gauche de chaque point de l'écran
static u16 s_Weight[MAX_OUT_WIDTH];	// sa part, sur 256 (le reste au point suivant)

// Calcule l'agrandissement une fois pour toutes (cœur 0, avant l'émulation).
void CKernel::SetupDisplay (void)
{
	const char *pPixels = Option ("pixels", "tele");
	const char *pColors = Option ("couleurs", "tele");
	bool bTelePixels = strcmp (pPixels, "carres") != 0;
	bool bTeleColors = strcmp (pColors, "pures") != 0;
	int nSaturation = OptionDecimal ("saturation", 100);
	int nBrightness = OptionDecimal ("luminosite", 100);

	for (unsigned i = 0; i < 8; i++)
	{
		u8 *pRGB = s_Palette[i];
		for (unsigned c = 0; c < 3; c++)
		{
			pRGB[c] = bTeleColors ? s_TeleRGB[i][c] : (i >> c & 1) * 0xFF;
		}

		// Saturation autour du gris de même luminance, puis luminosité
		int nGray = (pRGB[0] * 77 + pRGB[1] * 150 + pRGB[2] * 29) >> 8;
		for (unsigned c = 0; c < 3; c++)
		{
			int nValue = nGray + (pRGB[c] - nGray) * nSaturation / 100;
			pRGB[c] = Clamp255 (nValue * nBrightness / 100);
		}

		const char *pValue = Option (s_ColorName[i]);	// rouge=FF2A1A
		if (pValue != 0)
		{
			char *pEnd;
			unsigned long nRGB = strtoul (pValue, &pEnd, 16);
			if (*pEnd == '\0' && pEnd - pValue == 6)
			{
				pRGB[0] = nRGB >> 16;
				pRGB[1] = nRGB >> 8;
				pRGB[2] = nRGB;
			}
			else
			{
				JournalPrintf ("Affichage : %s=%s ignore (RRGGBB attendu)\n",
					       s_ColorName[i], pValue);
			}
		}
	}

	int nSrcWidth, nSrcHeight;
	oric_machine_framebuffer (&nSrcWidth, &nSrcHeight);

	CBcmFrameBuffer *pFrameBuffer = mScreen.GetFrameBuffer ();
	unsigned nFBWidth  = pFrameBuffer->GetWidth ();
	unsigned nFBHeight = pFrameBuffer->GetHeight ();

	m_nScaleY = nFBHeight / nSrcHeight;
	if (m_nScaleY == 0)
	{
		m_nScaleY = 1;			// écran trop petit : image coupée en bas
	}
	m_nOutHeight = nSrcHeight * m_nScaleY;
	if (m_nOutHeight > nFBHeight)
	{
		m_nOutHeight = nFBHeight;
	}

	unsigned nWanted = nSrcWidth * m_nScaleY * (bTelePixels ? TELE_RATIO_X100 : 100) / 100;
	m_nOutWidth = nWanted;
	if (m_nOutWidth > nFBWidth)
	{
		m_nOutWidth = nFBWidth;
	}
	if (m_nOutWidth > MAX_OUT_WIDTH)
	{
		m_nOutWidth = MAX_OUT_WIDTH;
	}
	m_nOffsetX = (nFBWidth  - m_nOutWidth) / 2;
	m_nOffsetY = (nFBHeight - m_nOutHeight) / 2;

	// Point j de l'écran = intervalle [j, j+1[ * nSrcWidth / m_nOutWidth de la ligne de l'Oric
	for (unsigned j = 0; j < m_nOutWidth; j++)
	{
		u64 nStart = (u64) j * nSrcWidth * 65536 / m_nOutWidth;
		u64 nEnd   = (u64) (j + 1) * nSrcWidth * 65536 / m_nOutWidth;
		unsigned nCol = nStart >> 16;
		u64 nBoundary = (u64) (nCol + 1) << 16;
		s_SrcCol[j] = nCol;
		s_Weight[j] = (nEnd <= nBoundary || nCol + 1 >= (unsigned) nSrcWidth)
			      ? 256 : (unsigned) ((nBoundary - nStart) * 256 / (nEnd - nStart));
	}

	// Cadre noir autour de l'image
	u16 *pDest = (u16 *) (uintptr) pFrameBuffer->GetBuffer ();
	unsigned nPitch = pFrameBuffer->GetPitch () / sizeof (u16);
	for (unsigned y = 0; y < nFBHeight; y++)
	{
		memset (pDest + y * nPitch, 0, nFBWidth * sizeof (u16));
	}

	JournalPrintf ("Affichage : framebuffer %ux%u, image %ux%u, pixels=%s, couleurs=%s,"
		       " saturation=%d, luminosite=%d\n",
		       nFBWidth, nFBHeight, m_nOutWidth, m_nOutHeight,
		       bTelePixels ? "tele" : "carres", bTeleColors ? "tele" : "pures",
		       nSaturation, nBrightness);
	JournalPrintf ("Palette :");
	for (unsigned i = 0; i < 8; i++)
	{
		JournalPrintf (" %s=%02X%02X%02X", s_ColorName[i],
			       s_Palette[i][0], s_Palette[i][1], s_Palette[i][2]);
	}
	JournalPrintf ("\n");
	if (m_nOutWidth < nWanted || m_nOutHeight < (unsigned) nSrcHeight)
	{
		JournalPrintf ("Affichage : framebuffer trop petit, mettre %s dans cmdline.txt\n",
			       bTelePixels ? "width=590 height=448" : "width=240 height=224");
	}
}

void CKernel::Blit (void)
{
	CBcmFrameBuffer *pFrameBuffer = mScreen.GetFrameBuffer ();
	unsigned nPitch = pFrameBuffer->GetPitch () / sizeof (u16);
	u16 *pDest = (u16 *) (uintptr) pFrameBuffer->GetBuffer () + m_nOffsetY * nPitch + m_nOffsetX;

	int nSrcWidth, nSrcHeight;
	const u8 *pSrc = oric_machine_framebuffer (&nSrcWidth, &nSrcHeight);
	if (nSrcWidth > MAX_SRC_WIDTH)
	{
		return;
	}

	static u8 Line[MAX_SRC_WIDTH][3];	// ligne de l'Oric, palette appliquée

	for (unsigned y = 0; y < m_nOutHeight / m_nScaleY; y++)
	{
		const u8 *pIn = pSrc + y * nSrcWidth * 3;
		for (int x = 0; x < nSrcWidth; x++, pIn += 3)
		{
			const u8 *pRGB = pIn;
			unsigned r = pIn[0], g = pIn[1], b = pIn[2];
			if (   (r == 0 || r == 0xFF) && (g == 0 || g == 0xFF)
			    && (b == 0 || b == 0xFF))
			{
				pRGB = s_Palette[(r & 1) | (g & 2) | (b & 4)];
			}
			Line[x][0] = pRGB[0];
			Line[x][1] = pRGB[1];
			Line[x][2] = pRGB[2];
		}

		u16 *pOut = pDest + y * m_nScaleY * nPitch;
		for (unsigned j = 0; j < m_nOutWidth; j++)
		{
			const u8 *p0 = Line[s_SrcCol[j]];
			unsigned w = s_Weight[j];
			if (w == 256)
			{
				pOut[j] = COLOR16 (p0[0] >> 3, p0[1] >> 3, p0[2] >> 3);
			}
			else
			{
				const u8 *p1 = p0 + 3;
				unsigned r = (p0[0] * w + p1[0] * (256 - w)) >> 8;
				unsigned g = (p0[1] * w + p1[1] * (256 - w)) >> 8;
				unsigned b = (p0[2] * w + p1[2] * (256 - w)) >> 8;
				pOut[j] = COLOR16 (r >> 3, g >> 3, b >> 3);
			}
		}
		for (unsigned k = 1; k < m_nScaleY; k++)
		{
			memcpy (pOut + k * nPitch, pOut, m_nOutWidth * sizeof (u16));
		}
	}
}
