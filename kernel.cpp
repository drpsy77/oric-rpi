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
//   width=240 height=224          taille native de l'Oric (le GPU agrandit)
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
// Touches : F5 = reset, Maj+F5 = NMI, F8 = bouton Action de LOCI (comme
//           Phosphoric), F12 = écrit la mesure de performance dans oric/perf.txt.
//
#include "kernel.h"
#include "oric_machine.h"

#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/timer.h>
#include <circle/usb/usbhid.h>
#include <stdio.h>
#include <string.h>

#define FRAME_US        ORIC_FRAME_US               // une trame PAL Oric = 20 ms
#define MAX_SAMPLES     1024                        // > 882 (44,1 kHz / 50)
#define SOUND_QUEUE_MS  100
#define STATS_FRAMES    500                         // mesure toutes les 10 s

static const char FromOric[] = "oric";

static int16_t s_SoundBuf[MAX_SAMPLES * 2];         // stéréo entrelacé
static unsigned s_nSamplesFrame;                    // échantillons par trame

CKernel *CKernel::s_pThis = 0;

CKernel::CKernel (void)
:	CStdlibAppStdio (FromOric),
	m_pKeyboard (0),
	m_pSound (0),
	m_bAzerty (false),
	m_bResetRequest (false),
	m_bNMIRequest (false),
	m_bPerfRequest (false),
	m_bLociDown (false),
	m_bLociUp (false),
	m_bHIDChanged (false),
	m_ucHIDModifiers (0),
	m_bF8Held (false),
	m_nLastAvg (0), m_nLastMax (0), m_nLastSkipped (0),
	m_nWorst (0), m_nWindows (0)
{
	s_pThis = this;
	mActLED.Blink (5);	// show we are alive
}

CStdlibApp::TShutdownMode CKernel::Run (void)
{
	mLogger.Write (FromOric, LogNotice, "Phosphoric %s sur Circle", oric_machine_version ());

	const char *pKeyMap = mOptions.GetKeyMap ();
	m_bAzerty = pKeyMap != 0 && strcmp (pKeyMap, "FR") == 0;

	if (mScreen.GetWidth () != 240 || mScreen.GetHeight () != 224)
	{
		mLogger.Write (FromOric, LogWarning,
			       "Ecran %ux%u : mettre width=240 height=224 dans cmdline.txt",
			       mScreen.GetWidth (), mScreen.GetHeight ());
	}

	oric_media_t Media;
	memset (&Media, 0, sizeof Media);
	Media.loci = mOptions.GetAppOptionDecimal ("loci", 0) == 1;
	Media.loci_flash_dir = mOptions.GetAppOptionString ("lociflash", "oric/loci");
	Media.loci_usb_dir   = mOptions.GetAppOptionString ("lociusb", "oric/usb");

	// ROM système : rom= ; à défaut, le menu LOCI (si LOCI) ou le BASIC 1.1
	static char LociMenuROM[128];
	snprintf (LociMenuROM, sizeof LociMenuROM, "%s/locirom", Media.loci_flash_dir);
	Media.rom_file = mOptions.GetAppOptionString ("rom",
				Media.loci ? LociMenuROM : "oric/basic11b.rom");
	Media.tape_file     = mOptions.GetAppOptionString ("tape");
	Media.fdc_timing    = mOptions.GetAppOptionString ("fdctiming");	// fast | real
	const char *pDisk    = mOptions.GetAppOptionString ("disk");
	const char *pDiskROM = mOptions.GetAppOptionString ("diskrom");
	if (pDisk != 0 && pDiskROM != 0)	// Microdisc seulement avec ROM + disquette
	{
		Media.disk_rom_file = pDiskROM;
		Media.disk_files[0] = pDisk;
	}

	if (!oric_machine_init (&Media))
	{
		mLogger.Write (FromOric, LogPanic, "Initialisation de l'Oric impossible");
	}

	oric_machine_quiet_log ();	// plus de journal Phosphoric par-dessus l'image

	s_nSamplesFrame = oric_machine_audio_rate () * FRAME_US / 1000000;	// 882

	StartSound ();

	unsigned nDeadline = CTimer::GetClockTicks ();
	unsigned nFrames = 0, nEmuSum = 0, nEmuMax = 0, nSkipped = 0;
	bool bSkipBlit = false;

	while (true)
	{
		UpdateKeyboard ();

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
		if (m_bPerfRequest)
		{
			m_bPerfRequest = false;
			WritePerf ();
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

		// Émulation d'une trame + son correspondant
		unsigned nStart = CTimer::GetClockTicks ();
		oric_machine_run_frame ();
		if (m_pSound != 0)
		{
			oric_machine_audio (s_SoundBuf, s_nSamplesFrame);
			m_pSound->Write (s_SoundBuf, s_nSamplesFrame * 2 * sizeof (int16_t));
		}
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

		// Cadence 50 Hz : sur l'horloge du son si actif, sinon sur le timer
		if (m_pSound != 0)
		{
			unsigned nWaitStart = CTimer::GetClockTicks ();
			while (m_pSound->GetQueueFramesAvail () > 3 * s_nSamplesFrame)	// ~60 ms d'avance
			{
				// attente active : le son se vide à 44,1 kHz
				if (CTimer::GetClockTicks () - nWaitStart > 10 * FRAME_US)
				{
					// le son ne se vide plus (pas de récepteur HDMI audio ?)
					mLogger.Write (FromOric, LogWarning, "Son HDMI bloque, cadence sur le timer");
					m_pSound->Cancel ();
					m_pSound = 0;
					nDeadline = CTimer::GetClockTicks ();
					break;
				}
			}
		}
		else
		{
			nDeadline += FRAME_US;
			if ((int) (CTimer::GetClockTicks () - nDeadline) > 5 * FRAME_US)
			{
				nDeadline = CTimer::GetClockTicks ();	// trop en retard : on recale
			}
			while ((int) (CTimer::GetClockTicks () - nDeadline) < 0)
			{
				// attente active jusqu'à l'échéance
			}
		}
	}

	return ShutdownHalt;
}

// Repris de Circle sample/34-sounddevices (cas "sndhdmi") : le Pi Zero
// n'a pas de sortie jack, le son passe par le HDMI.
void CKernel::StartSound (void)
{
	m_pSound = new CHDMISoundBaseDevice (&mInterrupt, oric_machine_audio_rate (), 384 * 10);

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
// Écrite sur demande (F12) dans oric/perf.txt, pour être lue sur un autre
// ordinateur. L'écriture sur la carte SD peut provoquer un petit à-coup.
void CKernel::WritePerf (void)
{
	FILE *pFile = fopen ("oric/perf.txt", "w");
	if (pFile == 0)
	{
		return;
	}
	fprintf (pFile, "Phosphoric %s sur Circle - temps d'emulation d'une trame\n",
		 oric_machine_version ());
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
	fprintf (pFile, "son HDMI              : %s\n", m_pSound != 0 ? "actif" : "inactif");
	fclose (pFile);
}

// ── Affichage ────────────────────────────────────────────────────────────
// Copie le framebuffer RGB888 de Phosphoric (240x224) dans le framebuffer
// 16 bits de Circle ; l'agrandissement à la taille de l'écran est fait par
// le GPU (width=240 height=224 dans cmdline.txt).
void CKernel::Blit (void)
{
	CBcmFrameBuffer *pFrameBuffer = mScreen.GetFrameBuffer ();
	u16 *pDest = (u16 *) (uintptr) pFrameBuffer->GetBuffer ();
	unsigned nPitch = pFrameBuffer->GetPitch () / sizeof (u16);

	int nSrcWidth, nSrcHeight;
	const u8 *pSrc = oric_machine_framebuffer (&nSrcWidth, &nSrcHeight);

	unsigned nWidth  = nSrcWidth;
	unsigned nHeight = nSrcHeight;
	if (nWidth > pFrameBuffer->GetWidth ())
	{
		nWidth = pFrameBuffer->GetWidth ();
	}
	if (nHeight > pFrameBuffer->GetHeight ())
	{
		nHeight = pFrameBuffer->GetHeight ();
	}

	for (unsigned y = 0; y < nHeight; y++)
	{
		const u8 *pLine = pSrc + y * nSrcWidth * 3;
		u16 *pOut = pDest + y * nPitch;
		for (unsigned x = 0; x < nWidth; x++)
		{
			pOut[x] = COLOR16 (pLine[0] >> 3, pLine[1] >> 3, pLine[2] >> 3);
			pLine += 3;
		}
	}
}
