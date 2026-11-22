//
// kernel.h — Kernel Circle de l'émulateur Oric (port de Phosphoric)
//
// Structure reprise de circle-stdlib samples/03-stdio-fatfs/kernel.h
// (GPL-3.0, (C) R. Stange / S. Mühlstrasser) ; voir SOURCES.md.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle_stdlib_app.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/cputhrottle.h>
#include <circle/multicore.h>
#include <circle/net/ntpdaemon.h>

class CKernel;

// Cible du journal de Circle une fois l'image de l'Oric affichée : ses
// messages défilaient sur l'écran (flashs par-dessus l'image). Seules les
// paniques (texte en gras, logger.cpp) y vont encore ; les autres restent
// dans la mémoire du logger, recopiée dans oric/journal.txt (LogToJournal).
class CPanicScreen : public CDevice
{
public:
	CPanicScreen (CDevice *pScreen) : m_pScreen (pScreen) {}
	int Write (const void *pBuffer, size_t nCount);

private:
	CDevice *m_pScreen;
};

// Cœur 1 : l'émulation, sans interruption (CMultiCoreSupport de Circle,
// voir doc/multicore.txt). Cœur 2 : lectures et écritures de fichiers pour
// TFTP et la page web (carte.cpp), endormi (WFE) entre deux. Le cœur 3
// s'arrête tout de suite.
class CEmuCores : public CMultiCoreSupport
{
public:
	CEmuCores (CKernel *pKernel);

	void Run (unsigned nCore);

private:
	CKernel *m_pKernel;
};

class CKernel : public CStdlibAppNetwork
{
public:
	CKernel (void);

	bool Initialize (void);
	TShutdownMode Run (void);

	void RunEmulation (void);		// cœur 1

private:
	void StartSound (void);
	void UpdateKeyboard (void);
	void ApplyRequests (void);
	void SetupDisplay (void);
	void UpdateNetwork (void);
	const char *NetworkName (void) const;	// « Wi-Fi » ou « Ethernet » (reseau=)
	void UpdateClock (void);
	void Blit (void);
	void WritePerf (void);
	unsigned Core0Time (unsigned nActivity, unsigned nStart);
	void Core0Late (unsigned nLate);
	void Core0Window (unsigned nNow);
	void LogToJournal (void);
	void ShowHalted (void);
	void SetupKeyMap (void);

	// Réglages : oric.cfg (dossier de lociflash=) d'abord, puis cmdline.txt
	void ReadSettings (void);
	static void WaitEmulationIdle (void);	// carte.cpp, TFTP, web (cœur 0)
	static void WebStatus (class CString *pHTML);	// page web : état
	static void WebReboot (void);			// page web : redémarrage
	const char *Option (const char *pName, const char *pDefault = 0);
	unsigned OptionDecimal (const char *pName, unsigned nDefault);

	// Repris de Circle sample/08-usbkeyboard (mode brut)
	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);

	// Souris USB (mouse1) : pour LOCI, comme sur la cartouche
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX,
					int nDisplacementY, int nWheelMove);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);
	void ApplyMouse (void);

private:
	// Pleine vitesse du processeur (sinon le firmware de la Pi 3 redescend
	// à 600 MHz) ; ralentit si la puce dépasse socmaxtemp (défaut 60 °C)
	CCPUThrottle m_CPUThrottle;

	CEmuCores m_EmuCores;
	CPanicScreen m_PanicScreen;

	// Modem Wi-Fi (modem=1), serveur TFTP (tftp=) : réseau démarré sans
	// attendre la connexion, en Wi-Fi ou en Ethernet (reseau=, mDeviceType)
	bool m_bModem;
	bool m_bTFTP;
	bool m_bNetStarting;
	bool m_bLinkUp;					// Ethernet : câble branché
	class COricTFTP *m_pTFTP;
	class COricMDNS *m_pMDNS;			// nom.local, noms en .local
	class COricWeb *m_pWeb;				// page web de la carte (web=)

	// Heure : NTP dès que le réseau est prêt (ntp=), fuseau de fuseau= appliqué
	// à l'horloge de Circle (dates des fichiers de la carte SD)
	CNTPDaemon *m_pNTPDaemon;
	unsigned m_nClockCheck;				// dernier contrôle du fuseau
	bool m_bClockSet;

	CUSBKeyboardDevice * volatile m_pKeyboard;
	CMouseDevice * volatile m_pMouse;

	// Souris : totaux tenus par le cœur 0 (interruption USB, seul écrivain),
	// le cœur 1 en tire les déplacements depuis la trame précédente (sans
	// remise à zéro, donc sans course entre les cœurs)
	volatile unsigned m_nMouseButtons;
	volatile int m_nMouseX, m_nMouseY, m_nMouseWheel;
	unsigned m_nMouseLastButtons;			// cœur 1
	int m_nMouseLastX, m_nMouseLastY, m_nMouseLastWheel;
	CSoundBaseDevice *m_pSound;

	// Affichage (pixels=, couleurs=...), calculé par SetupDisplay
	unsigned m_nScaleY;				// répétition de chaque ligne
	unsigned m_nOutWidth, m_nOutHeight;		// taille de l'image dans le framebuffer
	unsigned m_nOffsetX, m_nOffsetY;		// position (centrée)

	// Demandes du clavier (interruption, cœur 0), appliquées par le cœur 1
	volatile bool m_bResetRequest;
	volatile bool m_bNMIRequest;
	volatile bool m_bPerfRequest;			// celle-ci par le cœur 0
	volatile bool m_bRebootRequest;			// Ctrl+Alt+Suppr, cœur 0 aussi
	volatile bool m_bHaltRequest;			// Ctrl+Alt+Fin, cœur 0 aussi
	volatile bool m_bWebReboot;			// bouton de la page web (cœur 0)
	unsigned m_nWebRebootAt;			// ... à cet instant (GetClockTicks)

	// LOCI : bouton Action (F8) et rapport clavier HID
	volatile bool m_bLociDown;
	volatile bool m_bLociUp;
	volatile bool m_bHIDChanged;
	volatile unsigned char m_ucHIDModifiers;
	volatile unsigned char m_HIDKeys[6];
	bool m_bF8Held;

	// Échange entre les cœurs : le cœur 0 demande une trame quand le son en
	// a besoin, le cœur 1 la calcule (image + son) puis la déclare faite.
	// Entre les deux, le cœur 1 ne touche à rien : le cœur 0 peut alors
	// utiliser la carte SD (journal, F12) sans croiser l'émulation.
	volatile bool m_bMachineReady;
	volatile unsigned m_nFramesRequested;
	volatile unsigned m_nFramesDone;

	// Mesure de performance (F12 -> oric/perf.txt), tenue par le cœur 1
	unsigned m_nLastAvg, m_nLastMax, m_nLastSkipped;	// dernière fenêtre de 10 s
	unsigned m_nWorst, m_nWindows;				// depuis le démarrage
	unsigned m_nPerfCount;					// numéro de la mesure écrite

	// Cœur 0 (F12, journal) : retard des demandes de trame sur l'échéance de
	// 20 ms, et plus longues durées de ses activités. Un retard est rattrapé
	// par des trames demandées d'affilée : à-coups dans le rythme de l'Oric.
	enum
	{
		Act0Taches,		// mScheduler.Yield : réseau, TFTP, web, PHY...
		Act0Relais,		// NetRelayService
		Act0Divers,		// UpdateNetwork, UpdateClock, CPUThrottle
		Act0USB,		// UpdateKeyboard (UpdatePlugAndPlay, clés)
		Act0Son,		// Write du son HDMI
		Act0Journal,		// LogToJournal + JournalFlush (carte SD)
		Act0Nb
	};
	unsigned m_nAct0Max[Act0Nb];		// fenêtre de 10 s en cours
	unsigned m_nAct0Last[Act0Nb];		// fenêtre précédente
	unsigned m_nAct0Worst[Act0Nb];		// depuis le démarrage
	unsigned m_nAct0Request[Act0Nb];	// depuis la dernière demande de trame
	unsigned m_nLateMax, m_nLateLast, m_nLateWorst;	// retard (µs)
	unsigned m_nLateCount, m_nLateCountLast;	// trames en retard de > 5 ms
	unsigned m_nLateLogged;				// lignes du journal (> 10 ms)
	unsigned m_nRecal;				// recalages (> 5 trames)
	unsigned m_nCore0WindowStart, m_nCore0Windows;
	volatile unsigned m_nLastEmu;			// cœur 1 : durée de la dernière trame
	volatile unsigned m_nLastAudio;			// ... dont le son (ay_generate)
	volatile unsigned m_nLastBlit;			// cœur 1 : copie de l'image
	volatile unsigned m_nKeyReports;		// rapports du clavier USB reçus
	unsigned m_nKeyReportsLogged;			// ... à la ligne précédente
	unsigned m_nKeyReportsTicks;			// ... et son heure

	static CKernel *s_pThis;
};

#endif
