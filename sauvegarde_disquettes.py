#!/usr/bin/env python3
"""Sauvegarde des fichiers écrits sur les disquettes de la carte, avant que
installer_carte.sh ne les remplace par les nouvelles versions.

  sauvegarde_disquettes.py avant CARTE BUILD DOSSIER
  sauvegarde_disquettes.py apres CARTE BUILD DOSSIER

CARTE : dossier oric/ de la carte ; BUILD : build/ de cpa-oric ; DOSSIER :
sauvegarde/ du port (ignoré par Git). Disquettes vues : celles que
l'installation remplace, oric/usb/<nom>.dsk pour chaque build/<nom>.dsk et
oric/loci/cpa.dsk, cpa-en.dsk.

« avant » : chaque disquette de la carte est recopiée dans DOSSIER/ancien/
(dernière génération seulement), puis comparée à la référence, l'image
posée par l'installation précédente (DOSSIER/installe/) : un fichier
ajouté, ou modifié depuis, va dans DOSSIER/SAUVE.DSK. Sans référence
(première fois), la nouvelle image de build/ sert : seuls les fichiers
qu'elle n'a pas, ou non protégés (R/W) et différents, sont gardés.
SAUVE.DSK part de oric/usb/sauve.dsk de la carte s'il existe (on a pu y
écrire sur l'Oric), sinon de la copie de DOSSIER, sinon d'une disquette
vierge (sans système). Un nom déjà pris par un autre contenu reçoit un
chiffre (HOSTS -> HOSTS1).

« apres » : les images posées deviennent la référence ; SAUVE.DSK est
recopiée en oric/usb/sauve.dsk (montée en B: depuis le menu de LOCI).
"""
import os
import shutil
import sys

mkdisk = None                           # tools/mkdisk.py de cpa-oric (BUILD/../tools)

LOCI = ("cpa.dsk", "cpa-en.dsk")


def disquettes(carte, build):
    """(chemin relatif à oric/, image neuve de build/) des disquettes remplacées."""
    res = [("usb/" + n, os.path.join(build, n))
           for n in sorted(os.listdir(build)) if n.endswith(".dsk")]
    res += [("loci/" + n, os.path.join(build, n)) for n in LOCI]
    return res


def charge(chemin):
    try:
        return mkdisk.Disk.load(chemin)
    except SystemExit as e:
        print("  %s illisible (%s) : gardée telle quelle dans ancien/" % (chemin, e))
        return None


def nom_libre(sauve, nm, data):
    """Nom (11 caractères) pour data dans sauve : nm, ou nm suivi d'un chiffre."""
    base, ext = nm[:8].rstrip(), nm[8:]
    for k in range(10):
        n = nm if k == 0 else (base[:7] + str(k)).ljust(8) + ext
        if not sauve.find(n.encode("ascii")):
            return n
        if sauve.read_file(n.encode("ascii")) == data:
            return None                         # déjà sauvé tel quel
    raise SystemExit("SAUVE.DSK : trop de versions de %s" % mkdisk.nom_lisible(nm))


def avant(carte, build, dossier):
    os.makedirs(os.path.join(dossier, "ancien", "usb"), exist_ok=True)
    os.makedirs(os.path.join(dossier, "ancien", "loci"), exist_ok=True)
    chemin_sauve = os.path.join(dossier, "SAUVE.DSK")
    sur_carte = os.path.join(carte, "usb", "sauve.dsk")
    if os.path.exists(sur_carte):
        shutil.copyfile(sur_carte, chemin_sauve)
    if os.path.exists(chemin_sauve):
        sauve = mkdisk.Disk.load(chemin_sauve)
    else:
        sauve = mkdisk.Disk()
        sauve.format_dir()

    n = 0
    for rel, neuve in disquettes(carte, build):
        img = os.path.join(carte, rel)
        if not os.path.exists(img) or not os.path.exists(neuve):
            continue
        shutil.copyfile(img, os.path.join(dossier, "ancien", rel))
        d = charge(img)
        if d is None:
            continue
        ref_chemin = os.path.join(dossier, "installe", rel)
        premiere = not os.path.exists(ref_chemin)
        ref = charge(neuve if premiere else ref_chemin)
        if ref is None:
            continue
        cref = mkdisk.contenus(ref)
        for nm, (data, ro, _sy) in sorted(mkdisk.contenus(d).items()):
            if data is None or nm.endswith("$$$"):
                continue
            if nm in cref:
                if cref[nm][0] == data or (premiere and ro):
                    continue
                quoi = "modifié"
            else:
                quoi = "nouveau"
            dest = nom_libre(sauve, nm, data)
            if dest is None:
                continue
            sauve.write_file_raw(dest.encode("ascii"), data)
            print("  %-22s %-12s %s -> SAUVE.DSK %s" % (rel, mkdisk.nom_lisible(nm), quoi,
                                                        mkdisk.nom_lisible(dest)))
            n += 1
    sauve.save(chemin_sauve)
    print("Sauvegarde : %d fichier(s) ajouté(s) à %s" % (n, chemin_sauve))


def apres(carte, build, dossier):
    for rel, _neuve in disquettes(carte, build):
        img = os.path.join(carte, rel)
        if os.path.exists(img):
            ref = os.path.join(dossier, "installe", rel)
            os.makedirs(os.path.dirname(ref), exist_ok=True)
            shutil.copyfile(img, ref)
    chemin_sauve = os.path.join(dossier, "SAUVE.DSK")
    if os.path.exists(chemin_sauve):
        shutil.copyfile(chemin_sauve, os.path.join(carte, "usb", "sauve.dsk"))
        print("SAUVE.DSK remise sur la carte (oric/usb/sauve.dsk).")


def main(argv):
    global mkdisk
    if len(argv) != 5 or argv[1] not in ("avant", "apres"):
        print(__doc__)
        return 1
    sys.path.insert(0, os.path.join(os.path.abspath(argv[3]), "..", "tools"))
    import mkdisk
    (avant if argv[1] == "avant" else apres)(*argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
