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
#include <circle/sound/soundbasedevice.h>
#include <circle/cputhrottle.h>
#include <circle/multicore.h>

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
// voir doc/multicore.txt). Les cœurs 2 et 3 s'arrêtent tout de suite.
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
	void Blit (void);
	void WritePerf (void);
	void LogToJournal (void);
	void ShowHalted (void);

	// Réglages : oric.cfg (dossier de lociflash=) d'abord, puis cmdline.txt
	void ReadSettings (void);
	const char *Option (const char *pName, const char *pDefault = 0);
	unsigned OptionDecimal (const char *pName, unsigned nDefault);

	// Repris de Circle sample/08-usbkeyboard (mode brut)
	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);

private:
	// Pleine vitesse du processeur (sinon le firmware de la Pi 3 redescend
	// à 600 MHz) ; ralentit si la puce dépasse socmaxtemp (défaut 60 °C)
	CCPUThrottle m_CPUThrottle;

	CEmuCores m_EmuCores;
	CPanicScreen m_PanicScreen;

	// Modem Wi-Fi (modem=1) : réseau démarré sans attendre la connexion
	bool m_bModem;
	bool m_bNetStarting;

	CUSBKeyboardDevice * volatile m_pKeyboard;
	CSoundBaseDevice *m_pSound;
	bool m_bAzerty;

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

	static CKernel *s_pThis;
};

#endif
