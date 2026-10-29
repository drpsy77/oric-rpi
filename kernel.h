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

class CKernel : public CStdlibAppStdio
{
public:
	CKernel (void);

	TShutdownMode Run (void);

private:
	void StartSound (void);
	void UpdateKeyboard (void);
	void Blit (void);
	void WritePerf (void);

	// Repris de Circle sample/08-usbkeyboard (mode brut)
	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CUSBKeyboardDevice * volatile m_pKeyboard;
	CSoundBaseDevice *m_pSound;
	bool m_bAzerty;

	volatile bool m_bResetRequest;
	volatile bool m_bNMIRequest;
	volatile bool m_bPerfRequest;

	// Mesure de performance (F12 -> oric/perf.txt)
	unsigned m_nLastAvg, m_nLastMax, m_nLastSkipped;	// dernière fenêtre de 10 s
	unsigned m_nWorst, m_nWindows;				// depuis le démarrage

	static CKernel *s_pThis;
};

#endif
