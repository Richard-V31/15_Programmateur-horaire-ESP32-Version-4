// ===============================================================================================
//  PROGRAMMATEUR HORAIRE ESP32 - N RELAIS AVEC INTERFACE WEB (version flexible) ET MISE A JOUR OTA
// ================================================================================================
//  Ce programme transforme un ESP32 en programmateur horaire connecté :
//  - Il pilote un nombre CONFIGURABLE de relais indépendants (voir le tableau  "programmateurs[]" plus bas :
//    ajoutez/retirez une ligne pour changer le nombre de relais, aucune autre modification n'est nécessaire).
//  - Chaque relais peut fonctionner en mode AUTOMATIQUE (horaires programmés) ou en mode MANUEL (forçage ON/OFF par l'utilisateur)
//  - En mode automatique, chaque relais dispose d'une LISTE LIBRE de plages horaires (jusqu'à
//    MAX_PLAGES par relais, réglable d'une seule ligne) : chaque plage se règle à la MINUTE PRÈS
//    (ex: 06:33 -> 08:17), sans aucun arrondi ni découpage en créneaux, y compris à cheval sur
//    minuit. On les ajoute/supprime librement sur la page web, comme des lignes d'un formulaire.
//  - Une page web embarquée (servie directement par l'ESP32, sans carte SD ni système de fichiers pour le HTML/CSS/JS) 
//    permet de piloter et configurer le tout depuis un navigateur, sur le réseau local. La page s'adapte
//    automatiquement au nombre de relais déclarés dans le tableau ci-dessous.
//  - Les réglages (horaires, modes, états) sont sauvegardés dans la mémoire flash NVS (via la bibliothèque Preferences),
//    pour etre conservé en cas de coupures de courant.
//  - 🛟  RÉSEAU WIFI DE SECOURS "ESP32_Secours". Si la box ne répond plus (coupure générale,
//    box en panne...), l'ESP32 ouvre son PROPRE réseau WiFi : on s'y connecte avec un smartphone
//    et on retrouve la même page web à l'adresse http://192.168.5.1 (voir section
//    "RÉSEAU WIFI DE SECOURS"). Le réseau se referme tout seul quand la box est revenue. 🛟
//  - 🕒  RÉGLAGE MANUEL DE L'HEURE. Sans box, pas d'Internet donc pas d'heure NTP : un appui
//    sur l'horloge de la page web permet de recopier l'heure du smartphone en un clic, ou de
//    saisir une date/heure à la main. Dès que la box revient, l'heure NTP reprend la main.
//  -  RETOUR PLUS RAPIDE SUR LA BOX après une coupure : secours refermé 30 s après le
//    retour de la box même si un smartphone y est connecté (AP_FERMETURE_MEME_SI_CONNECTE),
//    recherche de la box toutes les 60 s avec un scan court, et nouvel essai immédiat d'une
//    box en "liste noire" si c'est le seul réseau connu visible.
//  -  COMMUTATION ENCORE PLUS RAPIDE vers le meilleur réseau connu quand la box revient,
//    même avec un smartphone connecté au secours : recherche toutes les 30 s, fermeture du
//    secours 15 s après la reconnexion (retour complet sur la box en moins d'une minute).
//  -  OTA FIABILISÉE : pendant une mise à jour, arrêt des scans WiFi, du serveur web et
//    du DNS de secours (qui pouvaient faire échouer le transfert) ; en cas d'échec, message sur
//    l'OLED puis redémarrage propre sur l'ancien programme ; et la page web se RECHARGE TOUTE
//    SEULE quand un nouveau firmware est détecté (sinon elle affichait l'ancienne interface).
//  -  COMMUTATION BOX <-> SECOURS STABLE SUR LA DURÉE : machine à états WiFi qui
//    restait bloquée après une reconnexion faite par loop() (d'où les cycles suivants plus
//    lents), plus d'arrêt complet de la radio WiFi à chaque perte de box (source probable des
//    redémarrages), ouverture du secours prioritaire sur les scans, et diagnostic "Dernier
//    redémarrage" + "Mémoire libre" dans la fenêtre Infos système.
//  -  détection de la perte de box accélérée (3 s au lieu de 6 s par défaut).
//  -  CORRECTIF "FAUX CONNECTÉ" : après une 1re bascule sur le secours,
//    WiFi.status() pouvait rester sur "connecté" pendant ~50 s alors que la box
//    était coupée (écran OLED : nom de réseau vide + IP 0.0.0.0). Le programme
//    ne cherchait donc ni la box ni à ouvrir le secours. La box n'est désormais
//    considérée connectée que si elle a AUSSI une vraie adresse IP (boxConnectee()).
//  -  après la perte de la box, UN SEUL scan avant l'ouverture du secours
//    (avant : un 2e scan démarrait pile au moment de l'ouvrir et la retardait de ~4 s).
//  -  suppression du journal WiFi de diagnostic (/journal), devenu inutile.
//  -  FIABILITÉ : un verrou (mutex FreeRTOS) protège les relais, les plages,
//    et la NVS, car le serveur web tourne dans une AUTRE tâche que loop().
//    Routes de commande EXPLICITES en POST (/set-mode, /set-state, /all) : un double appui
//    ou une requête rejouée ne peut plus inverser l'état par erreur (plus de "bascule").
//  -  JOURS DE LA SEMAINE par plage ("06:30-08:00/12345" = du lundi au vendredi,
//    1 = lundi ... 7 = dimanche ; sans "/" = tous les jours, compatible avec les réglages V3).
//  -  FORÇAGE TEMPORAIRE : ON ou OFF pendant 15 min, 1 h... ou jusqu'au prochain
//    changement programmé, puis RETOUR AUTOMATIQUE en mode AUTO (aussi après une coupure).
//  - : NOMS et SOUS-TITRES des programmations modifiables depuis la page web
//    (appui sur le nom), enregistrés en NVS.
//  - 🔆 LUMINOSITÉ DE L'ÉCRAN OLED réglable depuis la page web (Infos système >
//    "Luminosité de l'écran") : niveau de jour, MODE NUIT optionnel sur une plage horaire
//    (avec niveau propre, 0 % = écran éteint), aperçu en direct, enregistré en NVS.
// ==================================================================================================

// --- BIBLIOTHÈQUES ---
#include <WiFi.h>              // Gestion de la connexion WiFi de l'ESP32
#include "arduino_secrets.h"   // Fichier séparé contenant le nom du réseau (SSID), le mot de passe WiFi et le mot de passe OTA
#include <ESPAsyncWebServer.h> // Serveur web asynchrone (ne bloque pas la boucle principale)
#include <Preferences.h>       // Stockage clé/valeur en mémoire flash NVS (pour sauvegarder les réglages)
#include <ArduinoJson.h>       // Sérialisation/désérialisation JSON (lecture/écriture de config.json)
#include <time.h>              // Fonctions de gestion de l'heure système (NTP, strftime...)
#include <ESPmDNS.h>           // Permet d'accéder à l'ESP32 via un nom local (ex: http://richardv.local)
#include <DNSServer.h>         // Serveur DNS du réseau de secours (portail captif + richardv.local)
#include <esp_system.h>        // esp_reset_reason() (cause du dernier redémarrage)
#include <esp_wifi.h>          //  esp_wifi_scan_stop() (arrêt d'un scan WiFi pendant une OTA)
#include <esp_sntp.h>          // 🕒 Notification de synchronisation NTP (pour savoir si l'heure vient du NTP ou d'un réglage manuel)
#include <sys/time.h>          // 🕒 settimeofday() : réglage manuel de l'horloge interne
#include <ArduinoOTA.h>        // 🔄🛜Mise à jour du firmware par WiFi (OTA), sans câble USB
#include <Wire.h>              // Bus I2C, utilisé pour communiquer avec l'écran OLED
#include <Adafruit_GFX.h>      // Bibliothèque graphique de base (texte, formes...) pour l'écran OLED
#include <Adafruit_SSD1306.h>  // Pilote pour l'écran OLED SSD1306 (0.96" I2C)

// --- CONFIGURATION GÉNÉRALE ---

// 🔒Liste des réseaux WiFi connus (définis dans arduino_secrets.h). 
// L'ESP32 scannera les réseaux disponibles et se connectera à celui de cette liste
// qui offre le meilleur signal (RSSI). SECRET_SSID3/PASS3 sont optionnels :
// il suffit de les décommenter dans arduino_secrets.h pour ajouter un 3e ou un 4e réseau
struct WifiNetwork {
  const char* ssid;
  const char* pass;
};

// 🚩4️⃣ Types utilisés par la gestion WiFi non bloquante (voir plus bas, section
// "RECHERCHE ET CONNEXION AU MEILLEUR RÉSEAU WIFI CONNU"). Déclarés ici, tout
// en haut du fichier, à cause d'une contrainte de l'IDE Arduino : elle génère
// automatiquement les prototypes de toutes les fonctions du sketch et les
// insère près du début du fichier (avant le code qui suit) — un type
// personnalisé utilisé comme type de retour d'une fonction doit donc être
// défini AVANT ce point d'insertion, sous peine d'erreur de compilation
// ("'WifiConnResult' does not name a type").
enum WifiConnStep { WCS_IDLE, WCS_SCANNING, WCS_CONNECTING };
enum WifiConnResult { WCR_PENDING, WCR_CONNECTED, WCR_FAILED };
enum BetterNetStep { BNS_IDLE, BNS_SCANNING };


// 🚩5️⃣ Défini ici pour la même raison que les enums ci-dessus : "Plage" est
// utilisé comme type de paramètre par plusieurs fonctions (plageEstActive(),
// plagesVersTexte()...) et doit donc être connu AVANT le point où l'IDE
// Arduino insère automatiquement les prototypes de fonctions, sous peine
// d'erreur de compilation.
// Une plage horaire "libre" : un simple couple début/fin en minutes
// depuis minuit, SANS AUCUN arrondi ni découpage en créneaux. On peut donc
// saisir n'importe quelle heure à la minute près (ex: 06:33 -> 08:17).
struct Plage {
  int16_t debut; // Minute de début (0 à 1439). -1 = case inutilisée (pas de plage ici).
  int16_t fin;   // Minute de fin (1 à 1440 ; 1440 = "24:00", minuit en fin de plage).
                 // Si fin <= debut, la plage est à cheval sur minuit (ex: 22:00 -> 06:00).
  uint8_t jours; // Jours où la plage DÉMARRE : bit 0 = lundi ... bit 6 = dimanche
                 // (0x7F = tous les jours). Une plage à cheval sur minuit démarrée le
                 // vendredi se termine donc le samedi matin.
};
const uint8_t TOUS_LES_JOURS = 0x7F;

// 🚩6️⃣ Prototype explicite, pour la même raison que ci-dessus : la génération
// automatique des prototypes par l'IDE Arduino (basée sur ctags) peut échouer
// à repérer certaines fonctions - typiquement à cause du long texte HTML/CSS/JS
// brut embarqué plus bas dans le fichier (R"rawliteral(...)") qui perturbe son
// analyse. appliquerProgrammation() est appelée dans setup() AVANT sa
// définition textuelle (plus bas dans le fichier) : sans ce prototype, cela
// provoque l'erreur de compilation "'appliquerProgrammation' was not declared
// in this scope".
void appliquerProgrammation();

// 🔆 Prototypes explicites des fonctions de luminosité OLED, pour la même
// raison : l'IDE Arduino analyse aussi le JavaScript de la page web embarquée
// et peut confondre une fonction JS ("async function ...") avec une fonction
// C++ du même nom. ⚠️ Ne jamais donner à une fonction C++ le même nom qu'une
// fonction JavaScript de la page (erreur "'function' does not name a type").
void reglerLuminositeOLED(uint8_t pct);
bool estModeNuit();
uint8_t luminositeVoulue();
void gererLuminositeOLED();
void sauvegarderOled();

WifiNetwork knownNetworks[] = {
  { SECRET_SSID,  SECRET_PASS },
  { SECRET_SSID2, SECRET_PASS2 },
#ifdef SECRET_SSID3
  { SECRET_SSID3, SECRET_PASS3 },
#endif
};
const int knownNetworksCount = sizeof(knownNetworks) / sizeof(knownNetworks[0]);

// Nom d'hôte local : une fois connecté, l'ESP32 est joignable via
// 👨 http://richardv.local (en plus de son adresse IP), grâce au mDNS.
const char* hostname = "richardv";

// ============================================================================
//  🛟 RÉSEAU WIFI DE SECOURS "ESP32_Secours" (point d'accès intégré à l'ESP32)
// ============================================================================
//  Quand la box est injoignable, l'ESP32 crée son propre réseau WiFi. Il suffit
//  alors, sur le smartphone :
//    1) de se connecter au réseau WiFi "ESP32_Secours" (mot de passe AP_PASS) ;
//    2) d'ouvrir le navigateur à l'adresse  http://192.168.5.1
//  (⚠️ Android/iPhone peuvent signaler "pas d'accès Internet" : choisir
//  "Rester connecté", et si la page ne s'ouvre pas, couper les données mobiles.)
//  🔒 Mot de passe : 8 caractères minimum (exigence WPA2). Pour le changer sans
//  toucher à ce fichier, ajoutez dans arduino_secrets.h :
//    #define SECRET_AP_PASS "votre_mot_de_passe"
//  Le réseau est protégé : sans mot de passe, n'importe qui à portée pourrait
//  commander les relais (portail, garage...).
const char* AP_SSID = "ESP32_Secours";
#ifdef SECRET_AP_PASS
const char* AP_PASS = SECRET_AP_PASS;
#else
const char* AP_PASS = "12345678"; // 🔒 À personnaliser (min. 8 caractères)
#endif

// true  = réseau de secours ouvert EN PERMANENCE (même quand la box fonctionne)
// false = ouvert seulement quand la box est injoignable (recommandé)
const bool AP_SECOURS_TOUJOURS_ACTIF = false;

const unsigned long AP_DELAI_ACTIVATION    = 3000;  // Box perdue depuis 3 s -> ouverture du réseau de secours
const unsigned long AP_DELAI_DESACTIVATION = 3000; // Box retrouvée (et stable) depuis 3 s -> fermeture.
                                                    

// RETOUR PLUS RAPIDE SUR LA BOX
// Avant, le secours ne se refermait que si PLUS AUCUN smartphone n'y était
// connecté. Or, grâce à "l'Internet simulé", le smartphone se croit sur
// Internet et RESTE sur ESP32_Secours indéfiniment : le secours ne se
// fermait donc jamais. Désormais :
//   true  = fermeture même si un smartphone est connecté (il rebascule tout
//           seul sur la box, qu'il connaît) -> recommandé
//   false = ancien comportement (attend qu'aucun smartphone ne soit connecté)
const bool AP_FERMETURE_MEME_SI_CONNECTE = true;

// Pendant que le réseau de secours est ouvert, chaque recherche de la box
// (scan WiFi) perturbe brièvement la liaison avec le smartphone : on espace
// donc les tentatives, encore plus quand un smartphone est connecté.
const unsigned long WIFI_RETRY_NORMAL         = 5000;  // Pas de réseau de secours : nouvel essai toutes les 5 s
const unsigned long WIFI_RETRY_AP_SANS_CLIENT = 20000;  // Secours ouvert, personne dessus : toutes les 20 s
const unsigned long WIFI_RETRY_AP_AVEC_CLIENT = 10000;  // Smartphone connecté au secours : toutes les 10 s

// Scan "court" pendant le secours : durée d'écoute par canal Wi-Fi.
// 120 ms x 13 canaux = environ 1,6 s de perturbation (au lieu de ~4 s avec
// les 300 ms par défaut), ce qui permet de chercher la box plus souvent
// sans gêner le smartphone.
const uint32_t SCAN_MS_PAR_CANAL_SECOURS = 120;

bool apSecoursActif = false; // true = le réseau "ESP32_Secours" est actuellement ouvert

// instant (millis) où la box a été perdue (0 = box OK). Auparavant
// variable "static" interne à gererReseauSecours() ; rendue globale pour que
// loop() sache qu'une ouverture du secours est imminente et n'enchaîne pas
// un nouveau scan WiFi qui la retarderait (voir loop()).
unsigned long boxPerdueDepuis = 0;


// ============================================================================
//   PORTAIL CAPTIF DU RÉSEAU DE SECOURS
// ============================================================================
//  Pourquoi "richardv.local" ne marchait qu'une fois sur deux sur le réseau
//  de secours :
//   - les noms en ".local" (mDNS) sont mal gérés par beaucoup de smartphones
//     Android quand le réseau Wi-Fi n'a pas d'Internet ;
//   - le téléphone, voyant "pas d'Internet", envoie souvent ses requêtes par
//     les DONNÉES MOBILES au lieu du Wi-Fi : la page devient injoignable.
//  Solution : un mini serveur DNS sur l'ESP32 qui répond "192.168.5.1" à
//  TOUS les noms demandés (richardv.local compris), et une réponse de
//  "portail captif" aux tests de connexion des smartphones. Résultat :
//  dès la connexion à ESP32_Secours, le téléphone affiche "Se connecter au
//  réseau" et ouvre AUTOMATIQUEMENT la page du programmateur, en restant
//  sur le Wi-Fi de secours.
DNSServer dnsServer;

//  "INTERNET SIMULÉ" : éviter de devoir couper les données mobiles.
// Le smartphone vérifie s'il a Internet en appelant des adresses de test
// (Android : /generate_204, iPhone : /hotspot-detect.html, Windows :
// /connecttest.txt...). S'il conclut "pas d'Internet", il envoie ses requêtes
// par les DONNÉES MOBILES et la page 192.168.5.1 devient injoignable.
//   true  = l'ESP32 répond à ces tests EXACTEMENT comme le ferait Internet :
//           le téléphone garde le Wi-Fi de secours comme connexion principale.
//           (La page ne s'ouvre plus toute seule : taper 192.168.5.1.)
//   false = comportement V3.2 "portail captif" : la page s'ouvre toute seule,
//           mais il peut falloir couper les données mobiles.
const bool AP_INTERNET_SIMULE = true;
const byte DNS_PORT = 53;

// 🕒 Origine de l'heure courante (affichée dans la page web)
bool heureManuelle = false;  // true = heure réglée à la main depuis la page web (pas encore confirmée par le NTP)

const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3"; // Fuseau horaire (France, avec passage heure été/hiver automatique)

// ============================================================================
//   SIGNATURE DE BUILD Date en Français JJ-MM-AA  HH-MM(diagnostic OTA)
// ============================================================================
//  __DATE__/__TIME__ sont remplacés par le compilateur à la date/heure exacte
//  de la COMPILATION (pas du flash). Affichée au démarrage (Serial + écran
//  OLED) et exposée dans /get-info (popup "Infos système" de la page web) :
//  c'est le seul moyen fiable de vérifier, après une mise à jour OTA, que le
//  firmware qui tourne est bien le nouveau et pas l'ancien (masqué par la
//  NVS, par le cache du navigateur, ou par un flash parti sur le mauvais
//  port réseau). Recompilez avant CHAQUE upload OTA pour que cette date change.
const char* FIRMWARE_BUILD = __DATE__ " " __TIME__;

// ============================================================================
//  VERSION DE STRUCTURE DE CONFIGURATION NVS
// ============================================================================
//  FIRMWARE_BUILD sert uniquement au diagnostic OTA. Il change à chaque
//  compilation et ne doit donc jamais provoquer l'effacement des réglages.
//  La NVS n'est réinitialisée que si la structure des données change.
const uint16_t CONFIG_VERSION = 2; // Incrémenter uniquement si la structure NVS change réellement.

// ============================================================================
//  CONSERVATION DES RÉGLAGES LORS DES MISES À JOUR OTA
// ============================================================================
//  Piège classique : loadSettings() relit la NVS au démarrage et ECRASE les
//  valeurs par défaut du tableau programmateurs[] (heures, mode, état) dès
//  qu'une clé existe déjà en mémoire flash (ce qui est le cas dès qu'un
//  réglage a été sauvegardé une fois via la page web ou un bouton poussoir).
//  Résultat : vous changez un horaire ou un pin par défaut dans le code,
//  l'OTA se déroule parfaitement... mais l'ancien réglage stocké en NVS
//  peut masquer votre modification, ce qui est normal : la NVS contient les
//  réglages utilisateur et doit normalement rester prioritaire.
//
//  CORRECTIF : une simple nouvelle compilation ou mise à jour OTA NE VIDE
//  PLUS la NVS. Seul un changement volontaire de CONFIG_VERSION peut demander
//  une réinitialisation lorsque la structure des données devient incompatible.
//  La date/heure FIRMWARE_BUILD reste disponible dans "Infos système" pour
//  vérifier quel firmware est réellement installé.

// ============================================================================
//  ⚙️  ZONE DE CONFIGURATION DES RELAIS — C'EST ICI QUE VOUS ADAPTEZ LE
//      NOMBRE DE PROGRAMMATIONS / RELAIS À VOTRE CARTE
// ============================================================================
//  Chaque ligne du tableau ci-dessous représente UN relais piloté par l'ESP32.
//  Pour ajouter un relais : dupliquez une ligne, changez au minimum l'id
//  (unique, sans espace, ex: "12.") et la broche GPIO. Pour en retirer un,
//  supprimez la ligne correspondante. Aucune autre partie du code n'a besoin
//  d'être modifiée : la page web, les routes HTTP, la sauvegarde NVS et
//  l'écran OLED s'adaptent automatiquement au nombre de lignes ici présentes.
//
//
//  ⚠️ Broches GPIO utilisables en sortie sur un ESP32 DevKit classique :
//     4,5,13,14,16,17,18,19,21,22,23,25,26,27,32,33
//     (SDA=21 et SCL=22 sont déjà utilisés par l'écran OLED, ne pas les réutiliser)
//  ⚠️ À éviter : GPIO 34 à 39 (entrée seule, pas de sortie possible), et les
//     broches de boot (0, 2, 12, 15) qui peuvent perturber le démarrage si un
//     relais y est branché et tire la ligne à un état inattendu.
//  ⚠️ Nombre maximal réaliste : dépend surtout du nombre de broches GPIO
//     libres sur votre carte (une quinzaine sur un ESP32 classique). Au-delà,
//     il faut passer par un module d'extension I2C (ex: PCF8574) — code non
//     inclus ici mais la structure logique ci-dessous s'y prêterait bien.

// ============================================================================
//  RÉSOLUTION DE LA PROGRAMMATION : LISTE LIBRE DE PLAGES HORAIRES
// ============================================================================
//  Chaque relais dispose d'un petit tableau de MAX_PLAGES plages, chacune un
//  simple couple {debut, fin} exprimé en minutes depuis minuit (voir la
//  struct Plage, définie tout en haut du fichier). Il n'y a plus de grille ni
//  de créneaux : une plage peut commencer et finir à N'IMPORTE QUELLE MINUTE
//  (06:33 -> 08:17 est parfaitement valide), et le passage par minuit est géré
//  nativement (ex: 22:00 -> 06:00), sans découper la plage en deux.
//
//  👉⏱️5️⃣Pour changer le nombre de plages autorisées par relais, il suffit de
//     modifier MAX_PLAGES ci-dessous : la page web, le stockage NVS et
//     l'écran OLED s'adaptent automatiquement.
const int MAX_PLAGES = 6; // Nombre maximum de plages horaires personnalisées par relais

// Durée maximale d'un forçage temporaire (en minutes) : 24 h
const long DUREE_FORCAGE_MAX = 1440;

//  Longueur maximale (en caractères) d'un nom ou d'un sous-titre saisi sur la page web
const int MAX_LONGUEUR_NOM = 24;

struct Programmateur {
  const char* id;        // Identifiant unique utilisé dans les routes web et le stockage NVS (court, sans espace -> Ex: 1)
  const char* nom;       // Nom affiché sur la page web (ex: "Programmation 1")
  const char* sousNom;   // Sous-titre affiché sous le nom (ex: "Cuisine")
  const char* couleur;   // Couleur d'accent (code hexadécimal) pour ce relais dans l'interface
  int pin;               // Broche GPIO reliée au relais
  bool relayActiveHigh;  // true = HIGH active le relais, false = LOW active le relais
  const char* plagesDefaut; // Plages horaires PAR DÉFAUT, sous forme de texte :
                            // "09:00-10:30" pour une seule plage,
                            // "06:00-08:30,18:00-23:00" pour plusieurs (séparées par des virgules),
                            // "22:00-06:00" pour une plage à cheval sur minuit,
                            //  "06:30-08:00/12345" = du lundi au vendredi seulement
                            // (1 = lundi ... 7 = dimanche ; "/1-5", "/67" acceptés),
                            // "" pour aucune plage (relais éteint en mode auto).
                            // Les horaires sont pris à la minute près, SANS AUCUN ARRONDI
                            // (09:07 reste 09:07). Maximum MAX_PLAGES plages (les suivantes
                            // sont ignorées). Écrasées par la NVS dès qu'une programmation a
                            // été enregistrée depuis la page web.
  bool modeAuto;         // true = mode automatique (horaires) par défaut
  bool relayState;       // État par défaut (false = éteint)
  int pinBP;             // Broche GPIO du bouton poussoir de forçage physique (câblé entre GND et cette broche).
                         // -1 = pas de bouton poussoir pour ce programmateur.
  Plage plages[MAX_PLAGES]; // Plages réellement utilisées par le programme (liste libre, voir
                             // struct Plage). Remplies au démarrage à partir de plagesDefaut (voir
                             // initPlagesDefaut()), puis écrasées par la valeur enregistrée en NVS
                             // si elle existe. NE PAS les initialiser à la main dans le tableau
                             // ci-dessous : laissez la ligne se terminer sur la broche du bouton
                             // poussoir, comme avant.

  // Champs gérés UNIQUEMENT par le programme (ne pas les renseigner
  // dans le tableau programmateurs[] ci-dessous : ils démarrent vides / à false).
  String nomAff;              // Nom affiché (= nom par défaut, ou nom saisi sur la page web, en NVS)
  String sousNomAff;          // Sous-titre affiché (idem)
  bool forcageActif;          // true = forçage TEMPORAIRE en cours (le relais reste en mode AUTO)
  bool forcageEtat;           // État imposé pendant le forçage temporaire
  unsigned long forcageDebut; // millis() au début du forçage
  unsigned long forcageDuree; // Durée du forçage, en ms
};

// ============================================================================
//   VERROU (MUTEX) : PROTECTION CONTRE LES ACCÈS SIMULTANÉS
// ============================================================================
//  Le serveur web asynchrone (ESPAsyncWebServer) exécute ses routes dans sa
//  PROPRE tâche FreeRTOS (async_tcp), souvent sur l'autre cœur de l'ESP32,
//  EN MÊME TEMPS que loop(). Sans protection, une route /save pouvait réécrire
//  les plages pendant que loop() les lisait, ou deux sauvegardes NVS pouvaient
//  se chevaucher (bouton poussoir + page web). Toute lecture/écriture des
//  relais, des plages ou de la NVS se fait donc sous ce verrou.
//  Il est "récursif" : une fonction qui le tient peut en appeler une autre qui
//  le reprend (ex : une route qui appelle appliquerProgrammation()).
//  Usage : il suffit de déclarer "Verrou v;" au début d'un bloc : le verrou est
//  pris immédiatement et relâché AUTOMATIQUEMENT à la sortie du bloc.
SemaphoreHandle_t mutexProg = xSemaphoreCreateRecursiveMutex();
struct Verrou {
  Verrou()  { if (mutexProg) xSemaphoreTakeRecursive(mutexProg, portMAX_DELAY); }
  ~Verrou() { if (mutexProg) xSemaphoreGiveRecursive(mutexProg); }
};

// Centralise toutes les écritures vers les relais afin de respecter le
// niveau actif propre à chaque carte relais.
inline void ecrireRelais(const Programmateur &p, bool etat) {
  digitalWrite(p.pin, etat == p.relayActiveHigh ? HIGH : LOW);
}

// La box est-elle VRAIMENT connectée ? WiFi.status() seul ne suffit
// pas : après un passage par le réseau de secours, il peut rester bloqué sur
// WL_CONNECTED alors que la liaison est perdue (IP 0.0.0.0, SSID vide). On
// exige donc en plus une adresse IP valide. Utilisée PARTOUT à la place de
// "WiFi.status() == WL_CONNECTED".
bool boxConnectee() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}


//  🔘 Bouton poussoir de forçage physique (optionnel, uniquement câblé ici sur "1" à "4") :
//     câblage : une broche du bouton sur GND, l'autre sur la broche GPIO indiquée
//     (la broche interne est configurée en INPUT_PULLUP, pas de résistance externe nécessaire).
//     Chaque appui bref inverse l'état du relais ET bascule automatiquement en mode manuel,
//     exactement comme le bouton "FORCER ON/OFF" de la page web (voir checkPhysicalButtons()).
//
//  👉🚩Champs : { id, nom affiché, sous-titre, couleur (hex), broche GPIO,
//              niveau actif du relais (true = HIGH / false = LOW),
//              plages horaires par défaut (texte, plusieurs plages séparées par des virgules),
//              mode auto par défaut, état par défaut, broche GPIO du bouton poussoir (-1 = aucun) }

Programmateur programmateurs[] = {
  { "1", "Programmation 1", "Jardin",  "#FFFF00", 32, true, "06:21-08:10,09:00-13:04", true, false, 14 }, //#f59e0b est un code couleur hexadécimal R V B
  { "2", "Programmation 2", "Portail",  "#0CE892", 33, true, "07:05-09:10,17:08-19:33", true, false, 16 },
  { "3", "Programmation 3", "Extérieur", "#06b6d4", 25, true, "", true, false, 17 },  // "" ->aucune plage
  { "4", "Programmation 4", "Garage", "#ef4444", 26, true, "23:17-01:52", true, false, 18 }, 

  // 👉 Exemples de lignes supplémentaires à décommenter/adapter pour aller au-delà de 4 relais :
  //🔘 (mettre la broche du GPIO en dernier pour un programmateur avec bouton poussoir ou -1 pour sans Bouton poussoir)
  // { "5",  "Programmation 5",  "Relais 5",  "#a78bfa", 27, true, "12:00-22:00", true, false, -1 },
  // { "6",  "Programmation 6",  "Relais 6",  "#f472b6", 19, true, "13:00-23:00", true, false, -1 },
  // { "7",  "Programmation 7",  "Relais 7",  "#38bdf8", 4, true, "07:00-17:00", true, false, -1 },
  // { "8",  "Programmation 8",  "Relais 8",  "#fbbf24", 13, true, "",            true, false, -1 }, // "" = aucune plage au départ
};
const int NB_PROGRAMMATEURS = sizeof(programmateurs) / sizeof(programmateurs[0]);

// --- 🔘 ÉTAT INTERNE POUR L'ANTI-REBOND (DEBOUNCE) DES BOUTONS POUSSOIRS ---
// Un tableau par programmateur (même s'il n'a pas de bouton, pour garder les
// index en concordance avec programmateurs[]). Non utilisé si pinBP == -1.
const unsigned long BP_DEBOUNCE_MS = 40;        // Délai anti-rebond en millisecondes
bool bpDernierEtatLu[NB_PROGRAMMATEURS];       // Dernière lecture brute de la broche (avant stabilisation)
bool bpEtatStable[NB_PROGRAMMATEURS];          // Dernier état stabilisé (après anti-rebond)
unsigned long bpDerniereBascule[NB_PROGRAMMATEURS]; // Instant (millis) du dernier changement de lecture brute

// Recherche une Programmation par son id
// Renvoie null si l'id est inconnu.
Programmateur* findProg(const String &id) {
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    if (id == programmateurs[i].id) return &programmateurs[i];
  }
  return nullptr;
}

// ============================================================================
//  OUTILS DE MANIPULATION DES PLAGES HORAIRES LIBRES
// ============================================================================
//  Chaque plage est un simple couple {debut, fin} en minutes depuis minuit
//  (voir struct Plage). Aucun découpage en créneaux : les horaires sont
//  conservés à la minute près, exactement comme saisis.

// 🕒 Lecture de l'heure locale SANS ATTENTE. Par défaut, getLocalTime() attend
// jusqu'à 5 SECONDES quand l'heure n'est pas encore réglée : sans box (donc
// sans NTP), chaque appel figeait l'ESP32 5 s (page web qui ne répond plus,
// boutons poussoirs ignorés...). On limite ici l'attente à 10 ms.
bool lireHeureLocale(struct tm *t) {
  return getLocalTime(t, 10);
}

// Cause du dernier redémarrage, affichée dans "Infos système".
// Permet de savoir si un redémarrage vient d'un plantage, du chien de garde
// ou d'une chute de tension (alimentation trop faible pendant l'ouverture
// du point d'accès, qui consomme davantage).
const char* raisonRedemarrage() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Mise sous tension";
    case ESP_RST_SW:       return "Logiciel (OTA / restart)";
    case ESP_RST_PANIC:    return "Plantage (panic)";
    case ESP_RST_INT_WDT:  return "Chien de garde (IT)";
    case ESP_RST_TASK_WDT: return "Chien de garde (tache)";
    case ESP_RST_WDT:      return "Chien de garde";
    case ESP_RST_BROWNOUT: return "Chute de tension";
    case ESP_RST_EXT:      return "Bouton RESET";
    default:               return "Autre";
  }
}

// 🕒 Texte décrivant l'origine de l'heure : "ntp", "manuelle" ou "aucune".
const char* sourceHeure() {
  struct tm t;
  if (!lireHeureLocale(&t)) return "aucune";
  return heureManuelle ? "manuelle" : "ntp";
}

// 🕒 Appelée automatiquement par l'ESP32 à chaque synchronisation NTP réussie :
// l'heure redevient "officielle" et écrase un éventuel réglage manuel.
void ntpSynchronise(struct timeval *tv) {
  heureManuelle = false;
  Serial.println("Heure synchronisee par NTP");
}

// ============================================================================
//  TEMPS EN "MINUTES DE LA SEMAINE" (jours de la semaine)
// ============================================================================
//  Pour gérer les jours, l'instant présent est exprimé en minutes depuis le
//  LUNDI 00:00 : 0 = lundi 00:00, 1440 = mardi 00:00 ... 10079 = dimanche 23:59.
//  (Les plages, elles, restent exprimées en minutes depuis minuit + un masque
//  de jours : voir struct Plage.)
const int MIN_SEMAINE = 7 * 1440;
const char* const JOURS_COURTS[7] = { "Lu", "Ma", "Me", "Je", "Ve", "Sa", "Di" }; // Écran OLED
const char* const JOURS_LETTRES[7] = { "L", "Ma", "Me", "J", "V", "S", "D" };     // Résumés texte

// Ramène n'importe quelle valeur dans [0, MIN_SEMAINE[
int normSemaine(int w) { return ((w % MIN_SEMAINE) + MIN_SEMAINE) % MIN_SEMAINE; }

// Minute de la semaine correspondant à une heure locale (tm_wday : 0 = dimanche).
int minuteSemaine(const struct tm &t) {
  return ((t.tm_wday + 6) % 7) * 1440 + t.tm_hour * 60 + t.tm_min;
}

// Convertit une heure "HH:MM" en nombre de minutes depuis minuit (-1 si invalide).
// Contrôle STRICT (avant, "ab:cd" était lu comme 00:00).
int hmEnMinutes(const String &hmBrut) {
  String hm = hmBrut;
  hm.trim();
  int p = hm.indexOf(':');
  if (p < 1 || p > 2 || (int)hm.length() - p - 1 != 2) return -1;
  for (int i = 0; i < (int)hm.length(); i++) {
    if (i != p && !isDigit(hm[i])) return -1;
  }
  int h = hm.substring(0, p).toInt();
  int m = hm.substring(p + 1).toInt();
  // 24:00 est autorisé, mais pas 24:01, 24:30, etc.
  if (h < 0 || h > 24 || m < 0 || m > 59) return -1;
  if (h == 24 && m != 0) return -1;
  // Remarque : "24:00" renvoie volontairement 1440 (et non 0), pour qu'une
  // plage écrite "00:00-24:00" soit comprise comme la journée entière.
  return h * 60 + m;
}

// Convertit un nombre de minutes depuis minuit en "HH:MM".
String minutesEnHm(int m) {
  m = ((m % 1440) + 1440) % 1440;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", m / 60, m % 60);
  return String(buf);
}

// Fin de plage en texte : 1440 s'écrit "24:00" (et non "00:00").
String finEnHm(int fin) { return (fin == 1440) ? String("24:00") : minutesEnHm(fin); }

//  Jours : texte -> masque. Accepte des chiffres 1 (lundi) à 7 (dimanche),
// éventuellement avec des intervalles : "12345", "1-5", "67", "135", "6-1"
// (samedi, dimanche, lundi). Renvoie -1 si le texte est invalide.
// (Pas de virgule ici : la virgule sépare déjà les plages entre elles.)
int texteVersJours(const String &txt) {
  int masque = 0, prec = -1;
  bool intervalle = false;
  for (int i = 0; i < (int)txt.length(); i++) {
    char c = txt[i];
    if (c >= '1' && c <= '7') {
      int j = c - '1';
      if (intervalle) {
        for (int k = prec; k != j; k = (k + 1) % 7) masque |= (1 << k);
        intervalle = false;
      }
      masque |= (1 << j);
      prec = j;
    } else if (c == '-') {
      if (prec < 0 || intervalle) return -1;
      intervalle = true;
    } else if (c != ' ') {
      return -1;
    }
  }
  if (intervalle) return -1;
  return masque;
}

// Masque -> texte canonique pour la NVS / la page web : "12345" (vide = tous les jours).
String joursVersTexte(uint8_t jours) {
  if ((jours & TOUS_LES_JOURS) == TOUS_LES_JOURS) return String("");
  String out;
  for (int j = 0; j < 7; j++) if (jours & (1 << j)) out += char('1' + j);
  return out;
}

// Masque -> texte lisible ("L-V", "S-D", "L,Me,V") ; vide = tous les jours.
String joursLisibles(uint8_t jours) {
  if ((jours & TOUS_LES_JOURS) == TOUS_LES_JOURS) return String("");
  String out;
  int j = 0;
  while (j < 7) {
    if (!(jours & (1 << j))) { j++; continue; }
    int k = j;
    while (k + 1 < 7 && (jours & (1 << (k + 1)))) k++;
    if (out.length()) out += ",";
    out += JOURS_LETTRES[j];
    if (k > j) { out += "-"; out += JOURS_LETTRES[k]; }
    j = k + 1;
  }
  return out;
}

// Vide toutes les plages d'un relais (les marque comme inutilisées).
void plagesEffacer(Plage *p) {
  for (int i = 0; i < MAX_PLAGES; i++) { p[i].debut = -1; p[i].fin = -1; p[i].jours = 0; }
}

// Le jour "jour" (0 = lundi, -1 accepté = dimanche) fait-il partie du masque ?
inline bool jourDansMasque(uint8_t jours, int jour) {
  return jours & (1 << (((jour % 7) + 7) % 7));
}

// La plage "pl" est-elle active à la minute de la semaine "w" ?
// Une plage à cheval sur minuit (fin <= debut) appartient au jour où elle
// DÉMARRE : "22:00-06:00/5" = du vendredi 22:00 au samedi 06:00.
bool plageActiveA(const Plage &pl, int w) {
  if (pl.debut < 0) return false;
  w = normSemaine(w);
  int jour = w / 1440, m = w % 1440;
  if (pl.fin > pl.debut) return (m >= pl.debut && m < pl.fin) && jourDansMasque(pl.jours, jour);
  if (m >= pl.debut) return jourDansMasque(pl.jours, jour);      // Partie avant minuit
  if (m < pl.fin)    return jourDansMasque(pl.jours, jour - 1);  // Partie après minuit (démarrée la veille)
  return false;
}

// Le relais doit-il être ON à la minute de la semaine "w", d'après la liste de
// plages "p" ? (Parcourt les MAX_PLAGES cases, ignore celles inutilisées.)
bool plageEstActive(const Plage *p, int w) {
  for (int i = 0; i < MAX_PLAGES; i++) {
    if (plageActiveA(p[i], w)) return true;
  }
  return false;
}

// Copie dans "out" les plages définies, TRIÉES par heure de début croissante
// (tri à bulles : au plus MAX_PLAGES éléments, coût négligeable).
// Renvoie le nombre de plages copiées.
int trierPlages(const Plage *p, Plage *out) {
  int n = 0;
  for (int i = 0; i < MAX_PLAGES; i++) if (p[i].debut >= 0) out[n++] = p[i];
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - 1 - i; j++)
      if (out[j].debut > out[j + 1].debut) { Plage t = out[j]; out[j] = out[j + 1]; out[j + 1] = t; }
  return n;
}

//  Analyse UNE plage : "HH:MM-HH:MM" (tous les jours) ou
// "HH:MM-HH:MM/jours" (ex : "06:30-08:00/12345"). Renvoie true si elle est valide.
bool analyserPlage(String bloc, Plage &out) {
  bloc.trim();
  uint8_t jours = TOUS_LES_JOURS;
  int slash = bloc.indexOf('/');
  if (slash >= 0) {
    int j = texteVersJours(bloc.substring(slash + 1));
    if (j <= 0) return false; // Jours invalides ou aucun jour coché
    jours = (uint8_t)j;
    bloc = bloc.substring(0, slash);
    bloc.trim();
  }
  int tiret = bloc.indexOf('-');
  if (tiret <= 0) return false;
  int d = hmEnMinutes(bloc.substring(0, tiret));
  int f = hmEnMinutes(bloc.substring(tiret + 1));
  if (d < 0 || f < 0) return false;
  d %= 1440;           // "24:00" en début de plage = "00:00"
  if (f == d) return false; // Plage vide
  out.debut = d;
  out.fin = f;
  out.jours = jours;
  return true;
}

// Remplit un tableau de plages à partir d'un texte : "06:30-08:00,18:45-22:30/67".
// Utilisé pour les valeurs par défaut du tableau programmateurs[] (plagesDefaut),
// pour la NVS (loadSettings()) et pour les plages envoyées par la page web
// (route /save). Les plages invalides sont ignorées ; au-delà de MAX_PLAGES
// plages valides, les suivantes sont silencieusement ignorées.
void texteVersPlages(const char *txt, Plage *p) {
  plagesEffacer(p);
  if (!txt) return;
  String reste = String(txt);
  reste.trim();
  int idx = 0;
  while (reste.length() > 0 && idx < MAX_PLAGES) {
    int virgule = reste.indexOf(',');
    String bloc = (virgule < 0) ? reste : reste.substring(0, virgule);
    reste       = (virgule < 0) ? String("") : reste.substring(virgule + 1);
    Plage pl;
    if (analyserPlage(bloc, pl)) p[idx++] = pl;
  }
}

// Valide un texte de plages AVANT de remplacer la programmation (route /save) :
// une seule plage invalide, ou plus de MAX_PLAGES plages -> refus complet.
bool textePlagesValide(const String &txt) {
  String reste = txt;
  reste.trim();
  if (reste.length() == 0) return true; // Effacement volontaire de toutes les plages
  int nb = 0;
  while (reste.length() > 0) {
    int virgule = reste.indexOf(',');
    String bloc = (virgule < 0) ? reste : reste.substring(0, virgule);
    reste = (virgule < 0) ? String("") : reste.substring(virgule + 1);
    Plage pl;
    if (!analyserPlage(bloc, pl)) return false;
    if (++nb > MAX_PLAGES) return false;
  }
  return true;
}

// Sérialise TOUTES les plages définies (triées par heure de début), au format
// canonique "HH:MM-HH:MM,HH:MM-HH:MM/12345" (sans espace). C'est ce format,
// compact et facile à ré-analyser, qui est stocké en NVS et transmis tel quel
// au navigateur. Une plage valable tous les jours n'a pas de "/" : le format
// reste donc identique à celui de la V3 (réglages existants conservés).
String plagesVersTexteBrut(const Plage *p) {
  Plage tri[MAX_PLAGES];
  int n = trierPlages(p, tri);
  String out;
  for (int i = 0; i < n; i++) {
    if (i) out += ",";
    out += minutesEnHm(tri[i].debut);
    out += "-";
    out += finEnHm(tri[i].fin);
    String j = joursVersTexte(tri[i].jours);
    if (j.length()) { out += "/"; out += j; }
  }
  return out;
}

// Description lisible des plages, pour l'affichage : "06:30-08:00 (L-V), 11:30-13:15 +1".
// maxPlages = 0 -> toutes les plages ; sinon on n'en détaille que les
// premières et on ajoute "+n". avecJours = false -> sans les jours (écran OLED).
String plagesVersTexte(const Plage *p, int maxPlages, bool avecJours) {
  Plage tri[MAX_PLAGES];
  int n = trierPlages(p, tri);
  if (n == 0) return String("aucune plage");

  int affichees = (maxPlages <= 0) ? n : min(n, maxPlages);
  String out;
  for (int i = 0; i < affichees; i++) {
    if (i) out += ", ";
    out += minutesEnHm(tri[i].debut);
    out += "-";
    out += finEnHm(tri[i].fin);
    String j = joursLisibles(tri[i].jours);
    if (avecJours && j.length()) { out += " ("; out += j; out += ")"; }
  }
  if (n > affichees) { out += " +"; out += String(n - affichees); }
  return out;
}

// Nombre de minutes restant avant le PROCHAIN changement d'état du relais
// (ON->OFF ou OFF->ON), à partir de la minute de la semaine "w".
// Renvoie -1 si l'état ne change jamais (aucune plage, ou plages couvrant
// toute la semaine sans interruption).
// au lieu de tester les 10080 minutes de la semaine une par une, on
// ne teste que les instants où l'état PEUT changer (débuts et fins de plages,
// sur les 8 jours à venir), du plus proche au plus lointain.
int plagesProchainChangement(const Plage *p, int w) {
  w = normSemaine(w);
  bool etatInit = plageEstActive(p, w);
  int debutJour = w - (w % 1440);

  int cand[MAX_PLAGES * 2 * 8];
  int nc = 0;
  for (int i = 0; i < MAX_PLAGES; i++) {
    if (p[i].debut < 0) continue;
    int bornes[2] = { p[i].debut, p[i].fin % 1440 };
    for (int b = 0; b < 2; b++) {
      for (int d = 0; d <= 7; d++) {
        int delta = debutJour + d * 1440 + bornes[b] - w;
        if (delta < 1 || delta > MIN_SEMAINE) continue;
        // Insertion triée (au plus 96 valeurs)
        int k = nc++;
        while (k > 0 && cand[k - 1] > delta) { cand[k] = cand[k - 1]; k--; }
        cand[k] = delta;
      }
    }
  }
  for (int i = 0; i < nc; i++) {
    if (plageEstActive(p, w + cand[i]) != etatInit) return cand[i];
  }
  return -1;
}

//  Minutes avant le prochain DÉBUT de la plage "pl" (un jour où elle
// est programmée), à partir de la minute de la semaine "w". -1 si jamais.
int prochainDebutPlage(const Plage &pl, int w) {
  if (pl.debut < 0) return -1;
  w = normSemaine(w);
  int debutJour = w - (w % 1440);
  for (int d = 0; d <= 7; d++) {
    int t = debutJour + d * 1440 + pl.debut;
    if (t > w && jourDansMasque(pl.jours, t / 1440)) return t - w;
  }
  return -1;
}

// ============================================================================
//  PLAGE ACTIVE (fin) + PLAGE À VENIR (en entier) — RÉSUMÉ POUR
//  L'ÉCRAN OLED
// ============================================================================
//  L'état ON/OFF est indiqué par l'ID en vidéo inverse (voir printRelayLine()),
//  ce qui laisse 17 caractères pour :
//    - relais actuellement actif   : "08:00>11:30-13:15"
//      (s'éteint à 08:00, la plage suivante va de 11:30 à 13:15)
//    - relais pas encore actif     : "11:30-13:15"
//   si la plage suivante a lieu un AUTRE JOUR qu'aujourd'hui, son jour
//  est indiqué : "Ma 11:30-13:15" (inactif) ou "08:00>Ma11:30" (actif — la
//  fin de la plage suivante est alors omise faute de place).
String plageActiveEtSuivante(const Plage *p, int w) {
  if (w < 0) {
    // Heure pas encore synchronisée : ancien résumé "première plage (+n)",
    // faute de pouvoir situer "maintenant".
    String r = plagesVersTexte(p, 1, false);
    r.replace(" +", "+");
    return r;
  }

  w = normSemaine(w);
  int jourNow = w / 1440;
  bool activeNow = plageEstActive(p, w);

  int d1 = plagesProchainChangement(p, w);
  if (d1 < 0) return activeNow ? String("24h/24") : String("aucune plage");
  int t1 = w + d1;

  int d2 = plagesProchainChangement(p, t1);
  if (d2 < 0) return minutesEnHm(t1); // Sécurité : un seul changement trouvé
  int t2 = t1 + d2;

  if (!activeNow) {
    // t1 = début de la prochaine plage, t2 = sa fin : "11:30-13:15"
    String out;
    if (t1 / 1440 != jourNow) { out += JOURS_COURTS[(t1 / 1440) % 7]; out += " "; }
    out += minutesEnHm(t1);
    out += "-";
    out += minutesEnHm(t2);
    return out;
  }

  // Plage active : t1 = extinction, t2 = début de la plage suivante.
  String out = minutesEnHm(t1);
  out += ">";
  if (t2 / 1440 != jourNow) {
    out += JOURS_COURTS[(t2 / 1440) % 7];
    out += minutesEnHm(t2);
    return out;
  }
  out += minutesEnHm(t2);
  int d3 = plagesProchainChangement(p, t2);
  if (d3 >= 0) {
    out += "-";
    out += minutesEnHm(t2 + d3);
  }
  return out;
}

// ============================================================================
//   FORÇAGE TEMPORAIRE
// ============================================================================
//  Un forçage temporaire impose ON ou OFF pendant une durée donnée, PAR-DESSUS
//  la programmation : le relais reste en mode AUTO, et la programmation reprend
//  toute seule à la fin du forçage (voir appliquerProgrammation()). Il n'est pas
//  enregistré en NVS : après une coupure de courant, le relais revient donc
//  directement à sa programmation (comportement le plus sûr).
//  Basé sur millis() (et non sur l'heure) : fonctionne aussi sans heure réglée.

// Secondes restantes avant la fin du forçage (0 = terminé, -1 = pas de forçage).
long forcageRestantSec(const Programmateur &p) {
  if (!p.forcageActif) return -1;
  unsigned long ecoule = millis() - p.forcageDebut; // Correct même au passage à 0 de millis() (49 jours)
  if (ecoule >= p.forcageDuree) return 0;
  return (long)((p.forcageDuree - ecoule + 999) / 1000);
}

void demarrerForcage(Programmateur &p, bool etat, unsigned long dureeMin) {
  p.forcageActif = true;
  p.forcageEtat = etat;
  p.forcageDebut = millis();
  p.forcageDuree = dureeMin * 60000UL;
}

// Remplit les plages de chaque programmateur à partir de son champ
// plagesDefaut. Appelée dans setup() AVANT loadSettings(), pour que la valeur
// éventuellement enregistrée en NVS reprenne toujours le dessus.
void initPlagesDefaut() {
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    texteVersPlages(programmateurs[i].plagesDefaut, programmateurs[i].plages);
    programmateurs[i].nomAff = programmateurs[i].nom;        // (remplacés par la NVS si modifiés)
    programmateurs[i].sousNomAff = programmateurs[i].sousNom;
  }
}

// --- 🖥️CONFIGURATION DE L'ÉCRAN OLED (SSD1306, 0.96", I2C, adresse 0x3C) ---
// Câblage utilisé : broches I2C par défaut de l'ESP32 (SDA = GPIO21, SCL = GPIO22)
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1     // Pas de broche RESET dédiée (partagée avec le reset de l'ESP32)
#define SCREEN_ADDRESS 0x3C   // Adresse I2c de l'écran
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOK = false; // Passe à true si l'écran a été détecté correctement au démarrage

// Nombre de lignes "relais" affichables par page sur l'écran OLED 
// (le reste de l'écran est pris par l'en-tête réseau/IP). Si NB_PROGRAMMATEURS dépasse
// cette valeur, l'affichage bascule automatiquement en pages tournantes.
const int OLED_LIGNES_PAR_PAGE = 5;
const int OLED_CARS_PAR_LIGNE = 21; // Caractères par ligne (128 px / 6 px par caractère)

// ============================================================================
//  🔆 LUMINOSITÉ DE L'ÉCRAN OLED (réglable depuis la page web)
// ============================================================================
//  Deux niveaux en % (0 à 100), enregistrés en NVS :
//   - oledLumJour : luminosité normale (1 à 100 %) ;
//   - oledLumNuit : luminosité pendant la plage "nuit" (0 % = écran ÉTEINT,
//     ce qui limite aussi l'usure d'un OLED allumé 24 h/24).
//  Le mode nuit est optionnel (oledNuitActive) et défini par une plage
//  horaire à la minute près (oledNuitDebut -> oledNuitFin, en minutes depuis
//  minuit), éventuellement à cheval sur minuit (22:00 -> 07:00).
//  ⚠️ Les commandes I2C vers l'écran ne sont envoyées QUE depuis loop()
//  (voir gererLuminositeOLED()) : les routes web se contentent de modifier
//  ces variables, pour ne jamais parler à l'écran depuis deux tâches à la fois.
//  👉 Valeurs par défaut (premier démarrage) modifiables ici :
uint8_t oledLumJour    = 100;    // % en journée
uint8_t oledLumNuit    = 10;     // % la nuit (0 = écran éteint)
bool    oledNuitActive = false;  // Mode nuit désactivé par défaut
int16_t oledNuitDebut  = 22 * 60; // 22:00
int16_t oledNuitFin    = 7 * 60;  // 07:00

// Aperçu : pendant le réglage du curseur "nuit" sur la page web, ce niveau
// est appliqué quelques secondes pour voir le résultat en plein jour.
int           oledApercuNiveau = -1;  // -1 = pas d'aperçu en cours
unsigned long oledApercuDebut  = 0;
const unsigned long OLED_APERCU_MS = 6000; // Durée de l'aperçu
int           oledNiveauApplique = -1;     // Dernier niveau envoyé à l'écran (-1 = jamais)

// ============================================================================
//  PAGE WEB EMBARQUÉE (HTML + CSS + JAVASCRIPT)
// ============================================================================
//  Toute la page est stockée dans cette chaîne de caractères, placée en
//  mémoire flash (PROGMEM) plutôt qu'en RAM, et plutôt que dans un fichier
//  séparé sur un système de fichiers. Elle est envoyée telle quelle au
//  navigateur quand celui-ci demande la route "/".
//  IMPORTANT : cette page ne connaît PAS à l'avance le nombre de relais. Au
//  chargement, elle interroge la route "/get-config" pour savoir combien de
//  programmations existent et comment les afficher (nom, couleur...), puis
//  construit ses lignes dynamiquement. Vous pouvez donc changer le nombre de
//  relais dans le tableau "programmateurs[]" sans jamais toucher à ce code HTML.
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>ESP32 Control</title>
<style>
:root{
  --bg1:#0d0f16; --bg2:#141826; --card:rgba(255,255,255,.055); --line:rgba(255,255,255,.09);
  --txt:#eef1f6; --sub:#8891a0; --ok:#34d399; --off:#4b5568;
  /* 🎨 Couleurs du texte "Extinction dans..." / "Allumage dans..." (regroupées ici avec les autres couleurs) */
  --remain-on:#ff8787;  /* relais actuellement ON -> compte à rebours avant extinction */
  --remain-off:#2ecc71; /* relais actuellement OFF -> compte à rebours avant allumage */
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
html,body{height:100%;}
body{
  margin:0; font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;
  color:var(--txt);
  background:
    radial-gradient(ellipse 500px 300px at 15% -10%, rgba(99,102,241,.18), transparent 60%),
    radial-gradient(ellipse 500px 300px at 100% 0%, rgba(6,182,212,.14), transparent 55%),
    linear-gradient(180deg,var(--bg1) 0%,var(--bg2) 100%);
  display:flex; justify-content:center;
  padding:10px;
}
.page{width:100%; max-width:430px; display:flex; flex-direction:column; gap:8px;}

/* HEADER */
.top{
  display:flex; align-items:center; justify-content:space-between;
  padding:10px 14px; border-radius:16px;
  background:var(--card); border:1px solid var(--line); backdrop-filter:blur(10px);
}
.top .brand{display:flex; align-items:center; gap:10px;}
.top .brand-icon{
  width:34px; height:34px; border-radius:10px; font-size:16px; display:flex; align-items:center; justify-content:center;
  background:linear-gradient(135deg,#6366f1,#22d3ee);
}
.top h1{margin:0; font-size:.9rem; font-weight:800;}
.top p{margin:0; font-size:.62rem; color:var(--sub);}
.clock{font-family:"SFMono-Regular",Consolas,Menlo,monospace; font-size:1rem; font-weight:700; display:flex; align-items:center; gap:6px;}

/* QUICK ACTIONS */
.quick{display:grid; grid-template-columns:repeat(3,1fr); gap:6px;}
.qbtn{
  border:1px solid var(--line); background:var(--card); color:var(--txt);
/* 🚩Taille texte Tout ON Tout OFF et AUTO-> font-size:.66rem */
  border-radius:12px; padding:8px 4px; font-size:.85rem; font-weight:700;
  display:flex; flex-direction:column; align-items:center; gap:3px; cursor:pointer;
}
/* 🚩Taille des pastilles rouge et verte Tout ON Tout OFF <-font-size:1.6rem*/
.qbtn span.ic{font-size:1.6rem;}
.qbtn:active{transform:scale(.96);}
.qbtn.on:active, .qbtn.on{border-color:rgba(52,211,153,.5);}
.qbtn.off:active, .qbtn.off{border-color:rgba(251,113,133,.5);}
.qbtn.auto:active, .qbtn.auto{border-color:rgba(99,102,241,.5);}

/* RELAY ROWS */
.row{
  border-radius:16px; background:var(--card); border:1px solid var(--line);
  /* 🚨 Espace entre "Allumage/Extinction dans..." et le bas du bloc -> valeur en 3e position (bottom) */
  /* 1px → espace intérieur en haut du bloc */
  /* 12px → espace intérieur à droite  */
  /*  1px → espace intérieur en bas (marge entre "Allumage/Extinction" du bord inférieur)  */
  /* 12px → espace intérieur à gauche  */
  padding:11px 12px 1px 12px; backdrop-filter:blur(12px);
  border-left:3px solid var(--accent);
}
.row-top{display:flex; align-items:center; gap:8px;}
/* 🚨Taille couleur et position de Nom Affiché-Programmation 1 a 4 */
.name{font-size:1rem; font-weight:700; flex:1; min-width:0; color:#ffffff;text-align:center;}
/* 🚨Taille  couleur et position de sous titre-Relais 1 a 4 <-font-size:.99rem*/
.name small{display:block; font-size:.99rem; font-weight:500; color:#ffffff; text-align:center;}
.badge{font-size:.95rem; font-weight:900; min-width:34px; text-align:center;}
.badge.on{color:var(--ok); text-shadow:0 0 14px rgba(52,211,153,.5);}
.badge.off{color:#fb7185; text-shadow:0 0 12px rgba(251,113,133,.35);}
.badge.und{color:var(--off);}

/* 🚨VOYANT ROND ON/OFF (texte cerclé, fond coloré) */
.status-dot{
  width:34px; height:34px; border-radius:50%; flex-shrink:0; cursor:default;
  display:flex; align-items:center; justify-content:center;

  /* 🚩Taille du texte ON ou OFF->font-size:.9rem */
  font-size:.9rem; font-weight:900; letter-spacing:.02em; color:#fff;
  border:1px solid rgba(255,255,255,.15); transition:background .2s, box-shadow .2s;
}
/* 🚩Couleur du fond du bouton rond ON et OFF dans rgba(52,211,153,.6)*/
.status-dot.on{background:rgba(52,211,153,.9); box-shadow:0 0 14px rgba(52,211,153,.7); border-color:rgba(52,211,153,.9);}
.status-dot.off{background:rgba(237,92,92,0.9); box-shadow:0 0 12px rgba(237,92,92,0.7); border-color:rgba(237,92,92,0.9);}
.status-dot.und{background:rgba(255,255,255,.06); box-shadow:none; color:var(--sub);}

/* 🚩1️⃣ Bouton rectangulaire AUTO / MANUEL */
.mode-btn{
  min-width:92px; padding:8px 12px; border: 5px solid transparent; cursor:pointer;
  border-radius:12px; /* 🚨bords arrondis -> ajuster ce rayon */
  font-size:.85rem; font-weight:900; letter-spacing:.04em; text-transform:uppercase; color:#fff;
  text-align:center; transition:background .2s, box-shadow .2s;
}
/* 🚨Couleur fond et bordure Bouton mode AUTO Orange*/
.mode-btn.auto{
  background:rgba(251, 195, 12, 1);
  border-color: rgba(134, 119, 79, 1);
/* 🚨 Texte sombre (hérité de .mode-btn) : le fond orange clair
     offre un contraste trop faible avec du texte blanc (~1.6:1, sous le
     minimum WCAG AA de 3:1). Avec ce texte foncé, le contraste dépasse 12:1. */
  color:#1a1200;
}
/* 🚨Couleur fond et bordure Bouton mode MANUEL Vert */
.mode-btn.manuel{
  background:rgba(19, 151, 155, 1);
  border-color: rgba(15, 65, 73, 0.8);
  }

.row-body{display:flex; align-items:center; gap:8px; margin-top:8px;}
/* ⚠️ Les règles .tfield / input[type="time"] / .savebtn ci-dessous servaient
   aux anciens champs "Début"/"Fin" de chaque ligne. Elles sont conservées car
   les champs horaires de la fenêtre d'édition (saisie rapide d'une plage)
   s'appuient encore sur le style de input[type="time"]. */
.tfield{flex:1; display:flex; flex-direction:column; gap:2px;}
/* 🚨 Taille du texte Debut et Fin*/
/* Ancien .tfield label{font-size:.58rem; text-transform:uppercase; color:var(--sub); letter-spacing:.05em;} */
.tfield label{font-size:.80rem; text-transform:uppercase; color:#ffffff; letter-spacing:.05em;}
input[type="time"]{
/* 🚨AGRANDIR TEXTE HEURE PROG font-size:.78rem -> 1.2; */
  width:100%; padding:6px 6px; font-size:1.4rem; color:var(--txt);
  background:rgba(255,255,255,.06); border:1px solid var(--line); border-radius:9px; color-scheme:dark;
}
input[type="time"]:focus{outline:none; border-color:var(--accent);}
/* 🚨Position Bouton Sauvegarder; */
.savebtn {
  align-self: flex-end; border: none; cursor: pointer; border-radius: 9px; padding: 7px 10px;
  background: var(--accent); color: #0b0d12; font-size: .9rem; font-weight: 800; line-height: 1;
  margin-bottom: 9px; /* Ajustez margin-bottom (ex: 10px, 15px, 20px) pour le remonter plus ou moins */
}
/*  🚨Taille du texte ⚡ Forcer ON ou OFF -> font-size:.9rem */
.forcebtn{
  flex:1; border:1px solid var(--line); background:rgba(255,255,255,.05); color:var(--txt);
  border-radius:9px; padding:7px 10px; font-size:1.2rem; font-weight:700; cursor:pointer;
}
/* 🚨Taille du texte Sauvegardé ✓ -> msg{font-size:.85rem */
.forcebtn:active{background:rgba(var(--accent-rgb),.18); border-color:var(--accent);}
.msg{font-size:.85rem; color:#4ade80; height:12px; margin:2px 0 0; text-align:right;}
/* 🚨 Temps restant avant le prochain changement d'état (ON->OFF ou OFF->ON) ⏳ Allumage dans
   Couleur et taille de police : avant fixées en JS à chaque update(), maintenant
   ici en CSS (voir variables --remain-on/--remain-off dans :root) -> un seul
   endroit à modifier pour ajuster la taille ou les couleurs. */
.remain{font-size:1rem; text-align:center; margin:2px 0 0; min-height:0px;}
.remain.on{color:var(--remain-on);}
.remain.off{color:var(--remain-off);}

/* BLOC PROGRAMMATION : résumé des plages --------------------------------- */
/* 🚨 Taille du texte des heures programmées ("06:30-08:00, 18:45-22:30") */
.plages{
  flex:1; min-width:0; font-size:1rem; font-weight:700; color:#fff; text-align:center;
  padding:7px 8px; border-radius:9px; cursor:pointer;
  background:rgba(255,255,255,.06); border:1px solid var(--line);
}
.plages:active{border-color:var(--accent);}
/* Même code couleur que la barre ci-dessus, appliqué au texte de la plage
   correspondante dans le résumé ("06:30-08:00, 18:45-22:30"). */
.plages .rng-now{color:#22c55e;}
.plages .rng-next{color:#ef4444;}

/* FENÊTRE D'ÉDITION DES PLAGES (liste libre, plus de grille de cases) --- */
.grid-box{max-width:390px;}
.gr-head{font-size:.95rem; font-weight:800; text-align:center; margin:0 0 8px; color:var(--accent,#6366f1);}
.gr-resume{font-size:.8rem; text-align:center; color:var(--sub); margin:0 0 8px; min-height:1.1em;}
/* Zone défilante contenant les lignes de plages (jusqu'à MAX_PLAGES) */
.grid-body{max-height:46vh; overflow-y:auto; -webkit-overflow-scrolling:touch; padding-right:4px;}
/* 🚨 Une ligne = une plage libre : deux sélecteurs d'heure (à la minute
   près, step="60") + un bouton pour supprimer cette plage. */
.plage-row{display:flex; align-items:center; gap:6px; margin-bottom:8px;}
.fin-plage{display:flex;align-items:center;gap:6px;flex-wrap:wrap;}
.fin24{font-size:.78rem;color:var(--sub);white-space:nowrap;}
.fin24 input{accent-color:var(--ok);}

.plage-row input[type="time"]{flex:1; min-width:0; padding:6px 4px; font-size:1rem;}
.plage-row span.fleche{color:var(--sub); font-size:.95rem; flex-shrink:0;}
.plage-del{
  flex-shrink:0; width:30px; height:30px; border:1px solid var(--line); background:rgba(237,92,92,.12);
  color:#fb7185; border-radius:8px; font-size:1rem; font-weight:800; cursor:pointer;
}
.plage-vide{font-size:.8rem; text-align:center; color:var(--sub); padding:10px 0;}
/* 🚨 Bouton "Ajouter une plage" : désactivé une fois MAX_PLAGES atteint */
.plage-add{
  width:100%; margin-top:2px; border:1px dashed var(--line); background:transparent; color:var(--accent,#6366f1);
  border-radius:9px; padding:9px; font-size:.85rem; font-weight:800; cursor:pointer;
}
.plage-add:disabled{opacity:.4; cursor:default;}
.gr-tools{display:flex; gap:5px; margin:10px 0 6px;}
.gr-tools button{
  flex:1; border:1px solid var(--line); background:rgba(255,255,255,.05); color:var(--txt);
  border-radius:8px; padding:7px 4px; font-size:.72rem; font-weight:700; cursor:pointer;
}
.gr-actions{display:flex; gap:6px; margin-top:10px;}
.gr-actions button{border:none; cursor:pointer; border-radius:9px; padding:10px; font-size:1rem; font-weight:800; flex:1;}
.gr-cancel{background:rgba(255,255,255,.08); color:var(--txt);}
.gr-save{background:var(--ok); color:#06301f;}

.foot{text-align:center; font-size:.6rem; color:var(--sub); padding:4px 0 0;}

/* 🕒 RÉGLAGE DE L'HEURE ------------------------------------------------------ */
.clock{cursor:pointer;}
.clock.manuelle #time{color:#fbbf24;}  /* Heure réglée à la main : affichée en jaune */
.clock.aucune #time{color:#fb7185;}    /* Heure inconnue : affichée en rouge */
/* Bandeau d'alerte affiché quand l'ESP32 ne connaît pas l'heure */
.alerte-heure{
  display:none; border-radius:12px; padding:10px; text-align:center; cursor:pointer;
  font-size:.9rem; font-weight:800; color:#1a1200; background:#fbbf24;
}
.alerte-heure.show{display:block;}
.time-cur{font-family:"SFMono-Regular",Consolas,Menlo,monospace; font-size:1.6rem; font-weight:800; text-align:center; margin:2px 0;}
.time-src{font-size:.8rem; text-align:center; color:var(--sub); margin:0 0 12px;}
.time-btn{width:100%; border:none; cursor:pointer; border-radius:9px; padding:11px; font-size:.95rem; font-weight:800;}
.time-btn.phone{background:var(--ok); color:#06301f;}
.time-btn.manual{background:#6366f1; color:#fff; margin-top:8px;}
.time-sep{text-align:center; font-size:.75rem; color:var(--sub); margin:14px 0 8px;}
.time-input{
  width:100%; padding:8px; font-size:1.1rem; color:var(--txt); color-scheme:dark;
  background:rgba(255,255,255,.06); border:1px solid var(--line); border-radius:9px;
}
.time-msg{font-size:.85rem; text-align:center; min-height:1.2em; margin:8px 0 0; color:#4ade80;}

/* 📶 Regroupe le % de signal WiFi et le bouton Infos système, pour qu'ils
   se déplacent ensemble comme un seul bloc dans l'en-tête (.top). */
.wifi-indicator{display:flex; align-items:center; gap:6px; flex-shrink:0;}
/* 🚨Taille et couleur du texte "xx%" à côté du bouton Infos -> font-size:.7rem */
.wifi-pct{font-size:.7rem; font-weight:700; color:var(--sub); min-width:2.6em; text-align:right;}
/* 🎨 Couleur selon la qualité du signal (mêmes seuils que la plupart des OS) */
.wifi-pct.good{color:#00FF00;}       /* >= 67% : bon signal (vert)*/
.wifi-pct.mid{color:#fbbf24;}          /* 34-66% : signal moyen  (jaune)*/
.wifi-pct.weak{color:#fb7185;}         /* < 34% : signal faible  (rouge)*/

/* 🚨BOUTON INFO 🛜  width:30px; height:30px  Largeur Hauteur */
.info-btn{
  width:35px; height:35px; border-radius:50%; border:1px solid var(--line);
  background:var(--card); color:var(--txt); font-size:.85rem; font-weight:800;
  display:flex; align-items:center; justify-content:center; cursor:pointer; flex-shrink:0;
}
.info-btn:active{transform:scale(.92);}

/* POPUP INFOS SYSTEME */
.modal-overlay{
  position:fixed; inset:0; background:rgba(0,0,0,.55); backdrop-filter:blur(2px);
  display:none; align-items:center; justify-content:center; padding:16px; z-index:50;
}
.modal-overlay.show{display:flex;}
.modal-box{
  width:100%; max-width:340px; border-radius:16px; background:var(--bg2);
  border:1px solid var(--line); padding:16px; box-shadow:0 12px 40px rgba(0,0,0,.5);
}
.modal-box h2{margin:0 0 10px; font-size:.95rem; font-weight:800; display:flex; align-items:center; gap:8px;}
.modal-row{
  display:flex; justify-content:space-between; align-items:center; gap:10px;
  padding:7px 0; border-bottom:1px solid var(--line); font-size:.95rem;
}
.modal-row:last-of-type{border-bottom:none;}
.modal-row span.lbl{color:#F8F9FA;} /* Couleur du texte gauche Info systéme */
.modal-row span.val{font-weight:700; text-align:right; word-break:break-all;}
.modal-close{
  /* Fond IDENTIQUE pour tous les boutons "Fermer" (texte blanc toujours
     lisible, même avec une programmation jaune) ; la couleur de la
     programmation n'apparaît plus que dans la BORDURE (--accent).
     Fenêtres sans programmation (Infos système, Heure) : bordure neutre.
     🚨 Couleur du fond -> background ; épaisseur de la bordure -> border */

  margin-top:12px; width:100%; cursor:pointer; border-radius:9px; padding:9px;
  background:#2a3042; border:2px solid var(--accent, rgba(255,255,255,.25));
  color:#F8F9FA; font-size:1.2rem; font-weight:800;
}
.modal-close:active{background:#343b52;}

/* Nom de la programmation : appui = renommer */
.name{cursor:pointer;}
.name .crayon{font-size:.7rem; opacity:.45; margin-left:4px;}
/* Le crayon ✏️ est placé "hors flux" à droite du nom : il n'entre pas
   dans le calcul du centrage, donc le nom et le sous-titre sont centrés
   exactement l'un au-dessus de l'autre. */
.name .nom-txt{position:relative; display:inline-block;}
.name .nom-txt .crayon{position:absolute; left:100%; top:50%; transform:translateY(-50%); white-space:nowrap;}

/* Bouton "Forçage temporaire" (mode AUTO) */
.tempobtn{
  flex:1; border:1px dashed var(--line); background:transparent; color:var(--txt);
  border-radius:9px; padding:6px 10px; font-size:.9rem; font-weight:700; cursor:pointer;
}
/* Bandeau "Forcé ON/OFF — reprise auto dans ..." */
.tempo-on{
  display:flex; align-items:center; gap:8px; margin-top:8px; padding:7px 10px; border-radius:9px;
  background:rgba(251,191,36,.14); border:1px solid rgba(251,191,36,.5); color:#fde68a;
  font-size:.9rem; font-weight:700;
}
.tempo-on span{flex:1;}
.tempo-on button{
  border:none; cursor:pointer; border-radius:8px; padding:6px 10px; font-size:.8rem; font-weight:800;
  background:#fbbf24; color:#1a1200;
}

/* Jours de la semaine d'une plage (fenêtre d'édition) */
.plage-bloc{padding:6px 0 8px; border-bottom:1px solid var(--line); margin-bottom:6px;}
.jours-row{display:flex; gap:4px; margin-top:6px;}
.jour-btn{
  flex:1; min-width:0; padding:6px 0; border-radius:7px; cursor:pointer; font-size:.78rem; font-weight:800;
  border:1px solid var(--line); background:rgba(255,255,255,.04); color:var(--sub);
}
.jour-btn.on{background:var(--accent,#6366f1); border-color:var(--accent,#6366f1); color:#0b0d12;}
.jours-raccourcis{display:flex; gap:4px; margin-top:4px;}
.jours-raccourcis button{
  flex:1; border:none; background:transparent; color:var(--sub); font-size:.7rem; cursor:pointer; padding:2px;
  text-decoration:underline;
}
.plages .jours{font-weight:500; font-size:.8rem; opacity:.85;}

/* Fenêtres "Forçage temporaire" et "Renommer" */
.choix-etat{display:flex; gap:6px; margin-bottom:10px;}
.choix-etat button{
  flex:1; border:2px solid var(--line); background:rgba(255,255,255,.04); color:var(--txt);
  border-radius:10px; padding:10px; font-size:1rem; font-weight:900; cursor:pointer;
}
.choix-etat button.sel-on{border-color:var(--ok); background:rgba(52,211,153,.2);}
.choix-etat button.sel-off{border-color:#fb7185; background:rgba(251,113,133,.2);}
.durees{display:grid; grid-template-columns:repeat(3,1fr); gap:6px;}
.durees button{
  border:1px solid var(--line); background:rgba(255,255,255,.05); color:var(--txt);
  border-radius:9px; padding:10px 4px; font-size:.9rem; font-weight:700; cursor:pointer;
}
.durees button.large{grid-column:span 3;}
.duree-perso{display:flex; gap:6px; margin-top:8px;}
.duree-perso input{
  flex:1; min-width:0; padding:8px; font-size:1rem; color:var(--txt); color-scheme:dark;
  background:rgba(255,255,255,.06); border:1px solid var(--line); border-radius:9px;
}
.duree-perso button{
  border:none; border-radius:9px; padding:8px 12px; font-weight:800; cursor:pointer;
  background:#6366f1; color:#fff;
}
.champ-nom{display:flex; flex-direction:column; gap:4px; margin-bottom:10px;}
.champ-nom label{font-size:.8rem; color:var(--sub);}
.champ-nom input{
  padding:9px; font-size:1rem; color:var(--txt);
  background:rgba(255,255,255,.06); border:1px solid var(--line); border-radius:9px;
}
.note-modale{font-size:.72rem; color:var(--sub); text-align:center; margin:8px 0 0;}

/* 🔆 Luminosité de l’écran OLED */
.oled-btn{
  margin-top:12px; width:100%; cursor:pointer; border-radius:9px; padding:9px;
  background:rgba(251,191,36,.14); border:1px solid rgba(251,191,36,.5); color:#fde68a;
  font-size:1rem; font-weight:800;
}
.lum-bloc{padding:8px 0 10px; border-bottom:1px solid var(--line);}
.lum-bloc:last-of-type{border-bottom:none;}
.lum-tete{display:flex; justify-content:space-between; align-items:center; font-size:.95rem; font-weight:700; margin-bottom:6px;}
.lum-val{font-family:"SFMono-Regular",Consolas,Menlo,monospace; color:#fbbf24; min-width:4.5em; text-align:right;}
/* 🚨 Curseur de luminosité : couleur -> accent-color ; hauteur de la zone tactile -> height */
.lum-bloc input[type="range"]{width:100%; height:28px; accent-color:#fbbf24;}
.lum-nuit-opt{display:flex; align-items:center; gap:8px; font-size:.95rem; font-weight:700; cursor:pointer;}
.lum-nuit-opt input{width:20px; height:20px; accent-color:#6366f1;}
.lum-heures{display:flex; align-items:center; gap:6px; margin-top:8px;}
.lum-heures input[type="time"]{flex:1; min-width:0; font-size:1.1rem; padding:6px 4px;}
.lum-heures span{color:var(--sub);}
.lum-desactive{opacity:.4; pointer-events:none;}
.lum-etat{font-size:.8rem; text-align:center; color:var(--sub); margin:2px 0 6px;}
</style>
</head>
<body>
<div class="page">
  <div class="top">
    <div class="brand">
     <!-- <div class="brand-icon">⏱️</div> -->
      <div><h1>Programmation Horaire</h1></div>
    </div>
    <div class="wifi-indicator">
      <!-- 📶 Qualité du signal WiFi en %, mise à jour chaque seconde (voir update() plus bas) -->
      <button class="info-btn" onclick="openInfo()" title="Infos système">
<!-- 🚩 taille symbole 🛜 width="25" height="25" -->
        <svg viewBox="0 0 24 24" width="25" height="25" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M5 12.5a11 11 0 0 1 14 0"/>
          <path d="M8.3 16a6.5 6.5 0 0 1 7.4 0"/>
          <circle cx="12" cy="19.5" r="1.1" fill="currentColor" stroke="none"/>
        </svg>
      </button>
       <span class="wifi-pct" id="wifi-pct">--%</span>
    </div>
    <div class="clock" id="clock" onclick="openTime()" title="Régler l'heure"><span id="time">--:--</span></div>
  </div>

  <!-- 🕒 Bandeau affiché uniquement quand l'heure est inconnue (box/Internet coupés) -->
  <div class="alerte-heure" id="alerte-heure" onclick="openTime()">⚠️ Heure non réglée — appuyez ici pour la régler</div>

  <div class="quick">
    <button class="qbtn on" onclick="allOn()"><span class="ic">🟢</span>Tout ON</button>
    <button class="qbtn off" onclick="allOff()"><span class="ic">🔴</span>Tout OFF</button>
    <button class="qbtn auto" onclick="allAuto()"><span class="ic">🔁</span>Tout AUTO</button>
  </div>

  <div id="rows"></div>

  <div class="foot"><h1>Appuyez sur les horaires d'une programmation pour ajouter, modifier ou supprimer ses plages (à la minute près, jour par jour), puis validez avec <br>« Enregistrer ✓ ». Appuyez sur un nom pour le modifier.</h1></div>
</div>

<div class="modal-overlay" id="infoOverlay" onclick="if(event.target===this) closeInfo()">
  <div class="modal-box">
    <h2>📶 Infos système</h2>
    <div class="modal-row"><span class="lbl">WiFi</span><span class="val" id="info-wifi">---</span></div>
    <div class="modal-row"><span class="lbl">Box (SSID)</span><span class="val" id="info-ssid">---</span></div>
    <div class="modal-row"><span class="lbl">Nom (mDNS)</span><span class="val" id="info-host">---</span></div>
    <div class="modal-row"><span class="lbl">Adresse IP</span><span class="val" id="info-ip">---</span></div>
    <div class="modal-row"><span class="lbl">Adresse MAC</span><span class="val" id="info-mac">---</span></div>
    <div class="modal-row"><span class="lbl">Signal (RSSI)</span><span class="val" id="info-rssi">---</span></div>
    <div class="modal-row"><span class="lbl">Réseau secours</span><span class="val" id="info-ap">---</span></div>
    <div class="modal-row"><span class="lbl">Source heure</span><span class="val" id="info-hsrc">---</span></div>
    <div class="modal-row"><span class="lbl">Mise à jour du</span><span class="val" id="info-build">---</span></div>
    <!--  diagnostic des redémarrages -->
    <div class="modal-row"><span class="lbl">Dernier redémarrage</span><span class="val" id="info-reset">---</span></div>
    <div class="modal-row"><span class="lbl">Allumé depuis</span><span class="val" id="info-uptime">---</span></div>
    <div class="modal-row"><span class="lbl">Mémoire libre</span><span class="val" id="info-heap">---</span></div>
    <!-- 🔆 Accès au réglage de luminosité de l’écran OLED -->
    <button class="oled-btn" onclick="infoVersOled()">🔆 Luminosité de l’écran</button>
    <button class="modal-close" onclick="closeInfo()">Fermer</button>
  </div>
</div>

<!-- 🔆 Fenêtre "Luminosité de l’écran OLED" -->
<div class="modal-overlay" id="oledOverlay" onclick="if(event.target===this) closeOled()">
  <div class="modal-box">
    <h2>🔆 Luminosité de l’écran</h2>
    <p class="lum-etat" id="lum-etat">---</p>

    <div class="lum-bloc">
      <div class="lum-tete"><span>☀️ Jour</span><span class="lum-val" id="lum-jour-val">--</span></div>
      <input type="range" id="lum-jour" min="1" max="100" step="1"
             oninput="majLibellesLum()" onchange="apercuLum(this.value)">
    </div>

    <div class="lum-bloc">
      <label class="lum-nuit-opt">
        <input type="checkbox" id="lum-nuit-active" onchange="majLibellesLum()"> 🌙 Mode nuit
      </label>
      <div id="lum-nuit-zone">
        <div class="lum-heures">
          <input type="time" id="lum-debut" step="60">
          <span>→</span>
          <input type="time" id="lum-fin" step="60">
        </div>
        <div class="lum-tete" style="margin-top:10px;"><span>Luminosité la nuit</span><span class="lum-val" id="lum-nuit-val">--</span></div>
        <input type="range" id="lum-nuit" min="0" max="100" step="1"
               oninput="majLibellesLum()" onchange="apercuLum(this.value)">
      </div>
    </div>

    <p class="note-modale">En relâchant un curseur, l’écran affiche ce niveau quelques secondes (aperçu).<br>0 % la nuit = écran éteint.</p>
    <div class="gr-actions">
      <button class="gr-cancel" onclick="closeOled()">Annuler</button>
      <button class="gr-save" onclick="saveOled()">Enregistrer ✓</button>
    </div>
  </div>
</div>

<!-- 🕒 Fenêtre de réglage manuel de l'heure -->
<div class="modal-overlay" id="timeOverlay" onclick="if(event.target===this) closeTime()">
  <div class="modal-box">
    <h2>🕒 Réglage de l'heure</h2>
    <p class="time-cur" id="time-cur">--:--</p>
    <p class="time-src" id="time-src">---</p>
    <button class="time-btn phone" onclick="syncPhoneTime()">📱 Prendre l'heure du smartphone</button>
    <p class="time-sep">— ou saisie manuelle —</p>
    <input type="datetime-local" class="time-input" id="time-input" step="60">
    <button class="time-btn manual" onclick="setManualTime()">Appliquer cette date / heure</button>
    <p class="time-msg" id="time-msg"></p>
    <button class="modal-close" onclick="closeTime()">Fermer</button>
  </div>
</div>

<!-- Fenêtre d'édition des plages horaires : une LISTE LIBRE de plages
     (jusqu'à MAX_PLAGES), chacune réglable à la minute près, plus de grille
     de cases à cocher. Un seul exemplaire pour toute la page : elle est
     remplie à la volée avec les plages du relais sur lequel on vient
     d'appuyer (voir openGrid()). -->
<div class="modal-overlay" id="gridOverlay" onclick="if(event.target===this) closeGrid()">
  <div class="modal-box grid-box">
    <p class="gr-head" id="grid-title">---</p>
    <p class="gr-resume" id="grid-resume">---</p>

    <!-- Une ligne par plage (début + fin, sélecteurs d'heure step="60" = à
         la minute près) + bouton "✕" pour la supprimer. Générée par
         renderPlagesEdit(). -->
    <div class="grid-body" id="grid-body"></div>

    <!-- Désactivé automatiquement une fois MAX_PLAGES plages ajoutées -->
    <button class="plage-add" id="plage-add-btn" onclick="ajouterPlageLigne()">➕ Ajouter une plage</button>

    <div class="gr-tools">
      <button onclick="toutEffacerPlages()">Tout effacer</button>
    </div>

    <div class="gr-actions">
      <button class="gr-cancel" onclick="closeGrid()">Annuler</button>
      <button class="gr-save" onclick="saveGrid()">Enregistrer ✓</button>
    </div>
  </div>
</div>

<!--  Fenêtre "Forçage temporaire" -->
<div class="modal-overlay" id="tempoOverlay" onclick="if(event.target===this) closeTempo()">
  <div class="modal-box">
    <h2 id="tempo-titre">⏱ Forçage temporaire</h2>
    <div class="choix-etat">
      <button id="tempo-on" onclick="choisirEtatTempo(true)">ON</button>
      <button id="tempo-off" onclick="choisirEtatTempo(false)">OFF</button>
    </div>
    <div class="durees">
      <button onclick="lancerTempo(15)">15 min</button>
      <button onclick="lancerTempo(30)">30 min</button>
      <button onclick="lancerTempo(60)">1 h</button>
      <button onclick="lancerTempo(120)">2 h</button>
      <button onclick="lancerTempo(240)">4 h</button>
      <button onclick="lancerTempo(480)">8 h</button>
      <button class="large" onclick="lancerTempo('prochain')">Jusqu'au prochain changement programmé</button>
    </div>
    <div class="duree-perso">
      <input type="number" id="tempo-min" min="1" max="1440" placeholder="Autre durée (min)">
      <button onclick="lancerTempoPerso()">OK</button>
    </div>
    <p class="note-modale">Puis retour automatique en mode AUTO</p>
    <button class="modal-close" onclick="closeTempo()">Fermer</button>
  </div>
</div>

<!-- Fenêtre "Renommer une programmation" -->
<div class="modal-overlay" id="nomsOverlay" onclick="if(event.target===this) closeNoms()">
  <div class="modal-box">
    <h2>✏️ Renommer</h2>
    <div class="champ-nom">
      <label for="nom-input">Nom</label>
      <input type="text" id="nom-input" maxlength="24" autocomplete="off">
    </div>
    <div class="champ-nom">
      <label for="sous-input">Sous-titre</label>
      <input type="text" id="sous-input" maxlength="24" autocomplete="off">
    </div>
    <div class="gr-tools">
      <button onclick="nomsOrigine()">Noms d'origine</button>
    </div>
    <div class="gr-actions">
      <button class="gr-cancel" onclick="closeNoms()">Annuler</button>
      <button class="gr-save" onclick="saveNoms()">Enregistrer ✓</button>
    </div>
  </div>
</div>

<script>
// RELAYS est chargé dynamiquement depuis l'ESP32 (route /get-config) au lieu
// d'être codé en dur ici : la page s'adapte donc automatiquement au nombre de
// relais déclarés côté firmware (tableau programmateurs[]).
let RELAYS = [];
let lastData = null;
let timerUpdate = null;

// Nombre maximum de plages personnalisées par relais, transmis par l'ESP32
// (/get-config). 👉⏱️5️⃣ Un changement de MAX_PLAGES côté firmware suffit.
let MAX_PLAGES = 6;

let gridId = null;      // Id du relais en cours d'édition dans la fenêtre de plages
let gridPlages = [];    // Copie de travail : [{d:"06:30", f:"08:00", j:0x7F}, ...]

//  Jours de la semaine : bit 0 = lundi ... bit 6 = dimanche
const TOUS_JOURS = 0x7F;
const JOURS_BTN = ['L', 'M', 'M', 'J', 'V', 'S', 'D'];          // Boutons de la fenêtre d'édition
const JOURS_TXT = ['L', 'Ma', 'Me', 'J', 'V', 'S', 'D'];        // Résumés ("L-V", "S-D")
const JOURS_HORLOGE = ['Lun', 'Mar', 'Mer', 'Jeu', 'Ven', 'Sam', 'Dim']; // Jour affiché devant l'heure (en-tête)
const JOURS_LONGS = ['lundi', 'mardi', 'mercredi', 'jeudi', 'vendredi', 'samedi', 'dimanche'];

//  Les noms sont modifiables depuis la page : on les échappe avant de
// les insérer dans le HTML (un "<" dans un nom ne doit pas casser la page).
function esc(t) {
  return String(t).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

// Convertit une couleur hexadécimale ("#f59e0b") en triplet "r,g,b" pour les
// variables CSS --accent-rgb utilisées par les effets de survol/appui.
function hex2rgb(hex) {
  const h = hex.replace('#', '');
  const v = parseInt(h, 16);
  return `${(v >> 16) & 255},${(v >> 8) & 255},${v & 255}`;
}

// Envoi d'une commande POST (formulaire) ; lève une erreur avec le
// message de l'ESP32 si elle est refusée.
async function post(url, champs) {
  const body = new FormData();
  for (const k in champs) body.append(k, champs[k]);
  const res = await fetch(url, { method: 'POST', body: body });
  const txt = await res.text();
  if (!res.ok) throw new Error(txt || ('HTTP ' + res.status));
  return txt;
}

async function chargerConfig() {
  const res = await fetch('/get-config');
  const cfg = await res.json();
  RELAYS = cfg.relais;
  MAX_PLAGES = cfg.maxPlages;

  const rowsEl = document.getElementById('rows');
  rowsEl.innerHTML = RELAYS.map(r => `
    <div class="row" id="${r.id}-row" style="--accent:${r.color}; --accent-rgb:${hex2rgb(r.color)};">
      <div class="row-top">
        <div class="name" onclick="openNoms('${r.id}')" title="Renommer"><span class="nom-txt">${esc(r.name)}<span class="crayon">✏️</span></span><small>${esc(r.sub)}</small></div>
        <span id="${r.id}relay-status" class="status-dot und">---</span>
        <div>
          <button type="button" class="mode-btn" id="${r.id}mode-btn" onclick="toggleMode('${r.id}')">---</button>
        </div>
      </div>
      <div id="${r.id}timer-ui">
        <div class="row-body">
          <!-- Un appui sur les horaires ouvre la fenêtre de plages -->
          <div class="plages" id="${r.id}plages" onclick="openGrid('${r.id}')">---</div>
        </div>
        <!-- Forçage temporaire : bouton (si aucun en cours) ou bandeau -->
        <div class="row-body" id="${r.id}tempo-btn">
          <button class="tempobtn" onclick="openTempo('${r.id}')">⏱ Forçage temporaire</button>
        </div>
        <div class="tempo-on" id="${r.id}tempo-on" style="display:none;">
          <span id="${r.id}tempo-txt"></span>
          <button onclick="annulerTempo('${r.id}')">Annuler</button>
        </div>
      </div>
      <div class="row-body" id="${r.id}btn-ui" style="display:none;">
        <button class="forcebtn" onclick="forceState('${r.id}')">⚡ Forcer ON ou OFF</button>
      </div>
      <p class="remain" id="${r.id}remain"></p>
      <p class="msg" id="${r.id}msg"></p>
    </div>
  `).join('');
}

async function initRelays() {
  await chargerConfig();
  await update();
  if (!timerUpdate) timerUpdate = setInterval(update, 1000);
}

// 🚩 Temps restant avant le prochain changement d'état --------
// Convertit une heure "HH:MM" en nombre de minutes depuis minuit.
function hmToMin(hm) {
  const [h, m] = hm.split(':').map(Number);
  return h * 60 + m;
}

// Convertit un nombre de minutes depuis minuit en "HH:MM".
function minToHm(m) {
  m = ((m % 1440) + 1440) % 1440;
  return String(Math.floor(m / 60)).padStart(2, '0') + ':' + String(m % 60).padStart(2, '0');
}

// ==========================================================================
//   JOURS DE LA SEMAINE
// ==========================================================================
//  Format échangé avec l'ESP32 : "HH:MM-HH:MM" (tous les jours) ou
//  "HH:MM-HH:MM/12345" (1 = lundi ... 7 = dimanche).
function joursDepuisTexte(t) {
  if (!t) return TOUS_JOURS;
  let m = 0;
  for (const c of t) if (c >= '1' && c <= '7') m |= 1 << (c.charCodeAt(0) - 49);
  return m || TOUS_JOURS;
}
function joursVersTexte(m) {
  if ((m & TOUS_JOURS) === TOUS_JOURS) return '';
  let t = '';
  for (let j = 0; j < 7; j++) if (m & (1 << j)) t += String(j + 1);
  return t;
}
// Texte lisible : "" (tous les jours), "L-V", "S-D", "L,Me,V"
function joursLisibles(m) {
  if ((m & TOUS_JOURS) === TOUS_JOURS) return '';
  const morceaux = [];
  let j = 0;
  while (j < 7) {
    if (!(m & (1 << j))) { j++; continue; }
    let k = j;
    while (k + 1 < 7 && (m & (1 << (k + 1)))) k++;
    morceaux.push(k > j ? JOURS_TXT[j] + '-' + JOURS_TXT[k] : JOURS_TXT[j]);
    j = k + 1;
  }
  return morceaux.join(',');
}

// Découpe le texte de plages envoyé par l'ESP32 en [{d, f, j}, ...]
// (dans le MÊME ordre que l'ESP32, pour que les index "actives"/"suivante"
// correspondent).
function parsePlagesTexte(txt) {
  if (!txt) return [];
  return txt.split(',').map(bloc => {
    const [hor, jt] = bloc.split('/');
    const [d, f] = hor.split('-');
    return { d, f, j: joursDepuisTexte(jt) };
  }).filter(r => r.d && r.f);
}

// Résumé textuel ("06:30-08:00 (L-V), 18:45-22:30"), avec la ou les plages en
// cours en vert et la prochaine plage en rouge (voir .rng-now / .rng-next en
// CSS). Ces deux informations sont calculées par l'ESP32 (champs
// "actives" et "suivante" de /get-data), qui seul applique les jours.
function renderPlages(p, d) {
  const el = document.getElementById(p + 'plages');
  const plages = parsePlagesTexte(d.plages);
  if (plages.length === 0) { el.innerText = 'aucune plage'; return; }
  const actives = d.actives || [];
  el.innerHTML = plages.map((r, i) => {
    const jl = joursLisibles(r.j);
    const label = r.d + '-' + r.f + (jl ? ` <span class="jours">(${jl})</span>` : '');
    const cls = actives.includes(i) ? ' class="rng-now"'
              : i === d.suivante    ? ' class="rng-next"'
              : '';
    return `<span${cls}>${label}</span>`;
  }).join(', ');
}

// --- Fenêtre d'édition : liste libre de plages ----------------------------
function openGrid(p) {
  if (!lastData || !lastData[p]) return;
  gridId = p;
  // Copie de travail : rien n'est modifié tant qu'on n'enregistre pas.
  gridPlages = parsePlagesTexte(lastData[p].plages).map(r => ({ d: r.d, f: r.f, j: r.j }));
  const r = RELAYS.find(x => x.id === p);
  const box = document.querySelector('#gridOverlay .modal-box');
  box.style.setProperty('--accent', r.color);
  document.getElementById('grid-title').innerText = r.name + " - " + r.sub;
  renderPlagesEdit();
  document.getElementById('gridOverlay').classList.add('show');
}

function closeGrid() {
  document.getElementById('gridOverlay').classList.remove('show');
  gridId = null;
}

// Résumé texte affiché en haut de la fenêtre, recalculé au fil de l'édition.
function resumeDeTravail() {
  const valides = gridPlages.filter(r => r.d && r.f && r.d !== r.f && r.j);
  if (valides.length === 0) return "aucune plage";
  return valides.map(r => {
    const jl = joursLisibles(r.j);
    return r.d + '-' + r.f + (jl ? ' (' + jl + ')' : '');
  }).join(', ');
}

// Dessine la liste des plages (heures + jours), plus le bouton "Ajouter une
// plage" (désactivé une fois MAX_PLAGES atteint).
function renderPlagesEdit() {
  const body = document.getElementById('grid-body');
  if (gridPlages.length === 0) {
    body.innerHTML = '<p class="plage-vide">Aucune plage — appuyez sur « Ajouter une plage »</p>';
  } else {
    body.innerHTML = gridPlages.map((r, i) => {
      const fin24 = r.f === '24:00';
      const jours = JOURS_BTN.map((lettre, j) =>
        `<button class="jour-btn${(r.j & (1 << j)) ? ' on' : ''}" title="${JOURS_LONGS[j]}" onclick="basculerJour(${i}, ${j})">${lettre}</button>`
      ).join('');
      return `
      <div class="plage-bloc">
        <div class="plage-row">
          <input type="time" step="60" value="${r.d || ''}" onchange="modifierPlage(${i}, 'd', this.value)">
          <span class="fleche">→</span>
          <div class="fin-plage">
            <input type="time" step="60" value="${fin24 ? '' : (r.f || '')}" ${fin24 ? 'disabled' : ''} onchange="modifierPlage(${i}, 'f', this.value)">
            <label class="fin24"><input type="checkbox" ${fin24 ? 'checked' : ''} onchange="modifierFin24(${i}, this.checked)"> 24:00</label>
          </div>
          <button class="plage-del" onclick="supprimerPlageLigne(${i})" title="Supprimer cette plage">✕</button>
        </div>
        <div class="jours-row">${jours}</div>
        <div class="jours-raccourcis">
          <button onclick="fixerJours(${i}, 0x7F)">Tous</button>
          <button onclick="fixerJours(${i}, 0x1F)">Lun-Ven</button>
          <button onclick="fixerJours(${i}, 0x60)">Week-end</button>
        </div>
      </div>
    `;
    }).join('');
  }
  document.getElementById('grid-resume').innerText = resumeDeTravail();
  document.getElementById('plage-add-btn').disabled = gridPlages.length >= MAX_PLAGES;
}

//  Active / désactive un jour pour une plage
function basculerJour(i, j) {
  gridPlages[i].j ^= (1 << j);
  renderPlagesEdit();
}
function fixerJours(i, masque) {
  gridPlages[i].j = masque;
  renderPlagesEdit();
}

// input type="time" ne sait pas saisir "24:00" de façon portable : une case
// à cocher dédiée permet d'utiliser explicitement minuit comme fin de plage.
function modifierFin24(i, coche) {
  gridPlages[i].f = coche ? '24:00' : '';
  renderPlagesEdit();
}

// Appelé quand l'utilisateur change l'heure de début (champ 'd') ou de fin
// (champ 'f') d'une ligne : met à jour la copie de travail et le résumé.
function modifierPlage(i, champ, valeur) {
  gridPlages[i][champ] = valeur;
  document.getElementById('grid-resume').innerText = resumeDeTravail();
}

// Ajoute une nouvelle plage (tous les jours par défaut), jusqu'à MAX_PLAGES.
function ajouterPlageLigne() {
  if (gridPlages.length >= MAX_PLAGES) return;
  gridPlages.push({ d: '08:00', f: '12:00', j: TOUS_JOURS });
  renderPlagesEdit();
}

function supprimerPlageLigne(i) {
  gridPlages.splice(i, 1);
  renderPlagesEdit();
}

function toutEffacerPlages() {
  gridPlages = [];
  renderPlagesEdit();
}

// Enregistrement : une seule requête, qui envoie toutes les plages sous
// forme de texte ("06:30-08:00/12345,18:45-22:30").
async function saveGrid() {
  if (!gridId) return;
  const p = gridId;
  if (gridPlages.some(r => r.d && r.f && r.d !== r.f && !r.j)) {
    alert('Chaque plage doit avoir au moins un jour sélectionné.');
    return;
  }
  const texte = gridPlages
    .filter(r => r.d && r.f && r.d !== r.f)
    .map(r => {
      const jt = joursVersTexte(r.j);
      return r.d + '-' + r.f + (jt ? '/' + jt : '');
    })
    .join(',');
  try {
    await post('/save', { id: p, plages: texte });
    closeGrid();
    const msg = document.getElementById(p + 'msg');
    msg.innerText = "Sauvegardé ✓";
    setTimeout(() => msg.innerText = "", 2500);
    update();
  } catch (e) {
    console.error('Erreur d’enregistrement :', e);
    alert('Impossible d\'enregistrer la programmation : ' + e.message);
  }
}

// Bouton "Forcer ON ou OFF" (mode MANUEL) : on envoie l'état VOULU
// (l'inverse de celui affiché), et non plus une simple "bascule" : un double
// appui ou une requête rejouée ne peut plus inverser le relais deux fois.
async function forceState(id) {
  if (!lastData || !lastData[id]) return;
  try {
    await post('/set-state', { id: id, etat: lastData[id].etat ? 0 : 1 });
    await update();
  } catch (e) {
    console.error('Erreur de forçage :', e);
    alert('Impossible de commander le relais : ' + e.message);
  }
}

// Formate un nombre de minutes en texte lisible ("1 h 23 min" ou "45 min").
function formatRemainMin(totalMin) {
  const h = Math.floor(totalMin / 60);
  const m = totalMin % 60;
  return h > 0 ? `${h} h ${String(m).padStart(2, '0')} min` : `${m} min`;
}

// 📶 Met à jour le badge "xx%" à côté du bouton Infos système, avec une
// couleur selon la qualité du signal. pct = -1 (ou absent) -> non connecté.
function updateWifiPct(pct) {
  const el = document.getElementById('wifi-pct');
  if (!el) return;
  if (typeof pct !== 'number' || pct < 0) {
    el.textContent = '--%';
    el.className = 'wifi-pct';
    return;
  }
  el.textContent = pct + '%';
  el.className = 'wifi-pct ' + (pct >= 67 ? 'good' : pct >= 34 ? 'mid' : 'weak');
}

async function update() {
  try {
    const res = await fetch('/get-data');
    const data = await res.json();
    // nouveau firmware détecté (OTA) -> rechargement automatique de
    // la page, pour afficher la NOUVELLE interface.
    if (data.build) {
      if (!window.buildCharge) window.buildCharge = data.build;
      else if (data.build !== window.buildCharge) { location.reload(); return; }
    }
    lastData = data;
    document.getElementById('time').innerText =
      data.jour >= 0 ? JOURS_HORLOGE[data.jour] + ' ' + data.actuelle : data.actuelle; 
    // 🕒 Couleur de l'horloge selon l'origine de l'heure + bandeau d'alerte
    document.getElementById('clock').className = 'clock ' + (data.heureSource || '');
    document.getElementById('alerte-heure').classList.toggle('show', !data.heureOK);
    if (document.getElementById('timeOverlay').classList.contains('show')) majTimeModal();
    updateWifiPct(data.wifiPct);

    RELAYS.forEach(({ id: p }) => {
      const d = data[p];
      if (!d) return;
      const tempo = d.auto && d.forcage >= 0; //forçage temporaire en cours
      const modeBtn = document.getElementById(p + 'mode-btn');
      modeBtn.innerText = d.auto ? "AUTO" : "MANUEL";
      modeBtn.className = "mode-btn " + (d.auto ? "auto" : "manuel");
      document.getElementById(p + 'timer-ui').style.display = d.auto ? "block" : "none";
      document.getElementById(p + 'btn-ui').style.display = d.auto ? "none" : "flex";

      const status = document.getElementById(p + 'relay-status');
      status.innerText = d.etat ? "ON" : "OFF";
      status.className = "status-dot " + (d.etat ? "on" : "off");

      // Bouton ou bandeau de forçage temporaire
      document.getElementById(p + 'tempo-btn').style.display = tempo ? "none" : "flex";
      document.getElementById(p + 'tempo-on').style.display = tempo ? "flex" : "none";
      if (tempo) {
        document.getElementById(p + 'tempo-txt').innerText =
          "⏱ Forcé " + (d.etat ? "ON" : "OFF") + " — retour AUTO dans " + formatRemainMin(Math.ceil(d.forcage / 60));
      }

      // 🚨⏳ Temps restant avant le prochain changement d'état programmé
      // (uniquement en mode AUTO, hors forçage temporaire).
      const remainEl = document.getElementById(p + 'remain');
      if (d.auto && !tempo && d.restant >= 0) {
        remainEl.className = "remain " + (d.etat ? "on" : "off");
        remainEl.innerText = d.etat
          ? "⏰ Extinction dans " + formatRemainMin(d.restant)
          : "⏳ Allumage dans " + formatRemainMin(d.restant);
      } else {
        remainEl.className = "remain";
        remainEl.innerText = "";
      }

      // Résumé des plages (pas pendant l'édition de ce relais : la fenêtre
      // travaille sur une copie non encore enregistrée).
      if (p !== gridId) renderPlages(p, d);
    });
  } catch (e) { console.error("Erreur de synchronisation"); }
}

// 🚩 2️⃣ Bouton AUTO / MANUEL :  on envoie le mode VOULU (l'inverse de
// celui affiché) au lieu d'une bascule.
async function toggleMode(id) {
  if (!lastData || !lastData[id]) return;
  try {
    await post('/set-mode', { id: id, auto: lastData[id].auto ? 0 : 1 });
    update();
  } catch (e) {
    console.error('Erreur de changement de mode :', e);
    alert('Impossible de changer le mode : ' + e.message);
  }
}

// --- Actions groupées :  une seule commande traitée par l'ESP32 ---
async function toutCommander(action) {
  try { await post('/all', { action: action }); update(); }
  catch (e) { alert('Commande impossible : ' + e.message); }
}
function allOn()   { toutCommander('on'); }
function allOff()  { toutCommander('off'); }
function allAuto() { toutCommander('auto'); }

// ==========================================================================
//  FORÇAGE TEMPORAIRE
// ==========================================================================
let tempoId = null;
let tempoEtat = true;

function openTempo(p) {
  if (!lastData || !lastData[p]) return;
  tempoId = p;
  const r = RELAYS.find(x => x.id === p);
  document.getElementById('tempo-titre').innerText = '⏱ ' + r.name + ' - ' + r.sub;
  document.querySelector('#tempoOverlay .modal-box').style.setProperty('--accent', r.color);
  document.getElementById('tempo-min').value = '';
  choisirEtatTempo(!lastData[p].etat); // Par défaut : l'inverse de l'état actuel
  document.getElementById('tempoOverlay').classList.add('show');
}
function closeTempo() {
  document.getElementById('tempoOverlay').classList.remove('show');
  tempoId = null;
}
function choisirEtatTempo(on) {
  tempoEtat = on;
  document.getElementById('tempo-on').className = on ? 'sel-on' : '';
  document.getElementById('tempo-off').className = on ? '' : 'sel-off';
}
async function lancerTempo(duree) {
  if (!tempoId) return;
  const p = tempoId;
  try {
    await post('/set-state', { id: p, etat: tempoEtat ? 1 : 0, duree: duree });
    closeTempo();
    update();
  } catch (e) {
    alert('Forçage impossible : ' + e.message);
  }
}
function lancerTempoPerso() {
  const v = parseInt(document.getElementById('tempo-min').value, 10);
  if (!(v >= 1 && v <= 1440)) { alert('Durée entre 1 et 1440 minutes.'); return; }
  lancerTempo(v);
}
// "Annuler" = retour immédiat à la programmation (mode AUTO)
async function annulerTempo(p) {
  try { await post('/set-mode', { id: p, auto: 1 }); update(); }
  catch (e) { alert('Annulation impossible : ' + e.message); }
}

// ==========================================================================
//  RENOMMER UNE PROGRAMMATION
// ==========================================================================
let nomsId = null;

function openNoms(p) {
  const r = RELAYS.find(x => x.id === p);
  if (!r) return;
  nomsId = p;
  document.getElementById('nom-input').value = r.name;
  document.getElementById('sous-input').value = r.sub;
  document.getElementById('nomsOverlay').classList.add('show');
}
function closeNoms() {
  document.getElementById('nomsOverlay').classList.remove('show');
  nomsId = null;
}
function nomsOrigine() {
  const r = RELAYS.find(x => x.id === nomsId);
  if (!r) return;
  document.getElementById('nom-input').value = r.nameDef;
  document.getElementById('sous-input').value = r.subDef;
}
async function saveNoms() {
  if (!nomsId) return;
  try {
    await post('/set-noms', {
      id: nomsId,
      nom: document.getElementById('nom-input').value.trim(),
      sous: document.getElementById('sous-input').value.trim()
    });
    closeNoms();
    await chargerConfig(); // Reconstruit les lignes avec les nouveaux noms
    await update();
  } catch (e) {
    alert('Impossible d\'enregistrer les noms : ' + e.message);
  }
}

initRelays();

const MOIS_FR = {Jan:"Jan",Feb:"Fév",Mar:"Mar",Apr:"Avr",May:"Mai",Jun:"Juin",Jul:"Juil",Aug:"Août",Sep:"Sep",Oct:"Oct",Nov:"Nov",Dec:"Déc"};
function formatBuildFR(raw) {
  if (!raw) return "---";
  // 🎨 Les secondes (":SS") ne sont pas affichées : elles n'apportent rien
  // d'utile ici (on ne recompile pas deux fois à la même minute) et ça
  // libère encore de la place sur la ligne. La comparaison de firmware côté
  // ESP32 (storedBuild != FIRMWARE_BUILD) continue elle d'utiliser la chaîne
  // complète avec les secondes (non affectée par ce formatage d'affichage).
  const m = raw.match(/^(\w{3})\s+(\d{1,2})\s+(\d{4})\s+(\d{2}:\d{2}):\d{2}$/);
  if (!m) return raw;
  const [, mon, day, year, time] = m;
  // 🎨 Pas de " - " entre l'année et l'heure (juste un espace) : gagne encore
  // de la largeur pour que la date+heure tienne sur une seule ligne dans le
  // popup "Infos système" (colonne de droite, largeur limitée).
  return `${day.padStart(2,'0')} ${MOIS_FR[mon] || mon} ${year} ${time}`;
}

// --- Popup "Infos système" ---
async function openInfo() {
  const overlay = document.getElementById('infoOverlay');
  overlay.classList.add('show');
  try {
    const res = await fetch('/get-info');
    const info = await res.json();
    document.getElementById('info-wifi').innerText = info.connected ? "Connecté ✅" : "Déconnecté ❌";
    document.getElementById('info-ssid').innerText = info.ssid;
    document.getElementById('info-host').innerText = info.hostname;
    document.getElementById('info-ip').innerText = info.ip;
    document.getElementById('info-mac').innerText = info.mac;
    // 📶 dBm + % côte à côte, cohérent avec le badge de l'en-tête (voir updateWifiPct)
    document.getElementById('info-rssi').innerText =
      info.rssiPct >= 0 ? `${info.rssi} (${info.rssiPct}%)` : info.rssi;
document.getElementById('info-build').innerText = formatBuildFR(info.build);
    document.getElementById('info-reset').innerText = info.reset || '---';
    const up = info.uptime || 0;
    document.getElementById('info-uptime').innerText =
      Math.floor(up / 86400) + ' j ' + Math.floor(up % 86400 / 3600) + ' h ' + String(Math.floor(up % 3600 / 60)).padStart(2, '0') + ' min';
    document.getElementById('info-heap').innerText =
      Math.round(info.heap / 1024) + ' Ko (min ' + Math.round(info.heapMin / 1024) + ' Ko)';
    // 🛟 Réseau de secours + 🕒 origine de l'heure
    document.getElementById('info-ap').innerText = info.apActif
      ? `${info.apSsid} ✅ ${info.apIp} (${info.apClients} connecté${info.apClients > 1 ? 's' : ''})`
      : "Fermé (box OK)";
    document.getElementById('info-hsrc').innerText = libelleSourceHeure(info.heureSource);
  } catch (e) {
    document.getElementById('info-wifi').innerText = "Erreur";
  }
}
function closeInfo() {
  document.getElementById('infoOverlay').classList.remove('show');
}

// ==========================================================================
//  🔆 LUMINOSITÉ DE L'ÉCRAN OLED
// ==========================================================================
function majLibellesLum() {
  const j = document.getElementById('lum-jour').value;
  const n = parseInt(document.getElementById('lum-nuit').value, 10);
  document.getElementById('lum-jour-val').innerText = j + ' %';
  document.getElementById('lum-nuit-val').innerText = n === 0 ? 'éteint' : n + ' %';
  const actif = document.getElementById('lum-nuit-active').checked;
  document.getElementById('lum-nuit-zone').classList.toggle('lum-desactive', !actif);
}

// Bouton de la fenêtre Infos système : ferme cette fenêtre et ouvre celle
// de la luminosité. (Un seul appel dans onclick="..." : deux appels séparés
// par ";" y étaient pris pour du C++ par l'IDE Arduino.)
function infoVersOled() {
  closeInfo();
  openOled();
}

async function openOled() {
  document.getElementById('oledOverlay').classList.add('show');
  try {
    const res = await fetch('/get-oled');
    const o = await res.json();
    document.getElementById('lum-jour').value = o.jour;
    document.getElementById('lum-nuit').value = o.nuit;
    document.getElementById('lum-nuit-active').checked = o.nuitActive;
    document.getElementById('lum-debut').value = o.debut;
    document.getElementById('lum-fin').value = o.fin;
    document.getElementById('lum-etat').innerText = !o.ecran ? "⚠️ Écran OLED non détecté"
      : o.enNuit ? "Actuellement : mode nuit 🌙" : "Actuellement : mode jour ☀️";
    majLibellesLum();
  } catch (e) {
    document.getElementById('lum-etat').innerText = "Erreur de lecture des réglages";
  }
}
function closeOled() {
  document.getElementById('oledOverlay').classList.remove('show');
}

// Aperçu : l’écran prend ce niveau quelques secondes, sans rien enregistrer.
async function apercuLum(niveau) {
  try { await post('/oled-apercu', { niveau: niveau }); } catch (e) { console.error(e); }
}

async function saveOled() {
  const actif = document.getElementById('lum-nuit-active').checked;
  const debut = document.getElementById('lum-debut').value;
  const fin = document.getElementById('lum-fin').value;
  if (actif && (!debut || !fin || debut === fin)) {
    alert('Indiquez une heure de début et de fin différentes pour le mode nuit.');
    return;
  }
  try {
    await post('/set-oled', {
      jour: document.getElementById('lum-jour').value,
      nuit: document.getElementById('lum-nuit').value,
      nuitActive: actif ? 1 : 0,
      debut: debut || '22:00',
      fin: fin || '07:00'
    });
    closeOled();
  } catch (e) {
    alert("Impossible d’enregistrer la luminosité : " + e.message);
  }
}

// ==========================================================================
//  🕒 RÉGLAGE MANUEL DE L'HEURE
// ==========================================================================
function libelleSourceHeure(src) {
  if (src === 'ntp') return "Internet (NTP) ✅";
  if (src === 'manuelle') return "Réglée à la main ✋";
  return "Non réglée ⚠️";
}
function pad2(n) { return String(n).padStart(2, '0'); }

function majTimeModal() {
  if (!lastData) return;
  document.getElementById('time-cur').innerText = lastData.actuelle;
  document.getElementById('time-src').innerText = libelleSourceHeure(lastData.heureSource);
}

function openTime() {
  // Pré-remplit la saisie manuelle avec l'heure actuelle du smartphone
  const d = new Date();
  document.getElementById('time-input').value =
    `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}T${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  document.getElementById('time-msg').innerText = '';
  majTimeModal();
  document.getElementById('timeOverlay').classList.add('show');
}
function closeTime() {
  document.getElementById('timeOverlay').classList.remove('show');
}

async function envoyerHeure(body) {
  const res = await fetch('/set-time', { method: 'POST', body: body });
  const txt = await res.text();
  if (!res.ok) throw new Error(txt);
  document.getElementById('time-msg').innerText = "Heure réglée ✓";
  await update();
  majTimeModal();
}

// Envoie l'heure du smartphone en secondes UTC : aucun problème de fuseau.
async function syncPhoneTime() {
  const body = new FormData();
  body.append('epoch', Math.floor(Date.now() / 1000));
  try { await envoyerHeure(body); }
  catch (e) { alert("Impossible de régler l'heure : " + e.message); }
}

// Envoie la date/heure saisie (heure locale française), convertie par l'ESP32.
async function setManualTime() {
  const v = document.getElementById('time-input').value;
  if (!v) { alert("Choisissez d'abord une date et une heure."); return; }
  const body = new FormData();
  body.append('datetime', v);
  try { await envoyerHeure(body); }
  catch (e) { alert("Impossible de régler l'heure : " + e.message); }
}
</script>
</body>
</html>

)rawliteral";

// ============================================================================
//  SERVEUR WEB ET VARIABLES GLOBALES
// ============================================================================

AsyncWebServer server(80); // Instance du serveur web asynchrone, écoute sur le port 80 (HTTP standard)
Preferences preferences;   // Instance d'accès à la mémoire NVS (utilisée par saveSettings/loadSettings)

String now; // Heure courante au format "HH:MM", recalculée à chaque seconde dans loop()

// Traçabilité de la réinitialisation NVS après OTA, consultable depuis la
// page web (popup "Infos système") SANS avoir besoin du moniteur série — utile
// car celui-ci n'est pas accessible pendant/après une mise à jour par WiFi.
// Renseignées une seule fois, dans loadSettings(), au tout début du démarrage.
bool nvsReinitialiseeAuDemarrage = false; // true = la structure NVS a dû être réinitialisée à ce démarrage
String buildPrecedentNVS = "";            // Ancien FIRMWARE_BUILD lu en NVS avant ce démarrage ("" = tout premier démarrage)

// ============================================================================
//  SYSTÈME DE SAUVEGARDE / CHARGEMENT DES RÉGLAGES (Preferences / NVS)
// ============================================================================
//  La bibliothèque Preferences stocke des paires clé/valeur directement dans
//  la zone NVS (Non-Volatile Storage) de la mémoire flash de l'ESP32 — la
//  même zone qu'utilise en interne WiFi.begin() pour retenir les identifiants
//  réseau. Chaque groupe de réglages est rangé dans un "namespace" (ici
//  "config"), un peu comme un fichier .ini séparé. Les clés sont construites
//  dynamiquement à partir de l'id de chaque programmateur (ex: "1.pl"),
//  donc ce code fonctionne quel que soit le nombre de relais déclarés.
//  Note : NVS limite chaque clé à 15 caractères ; avec des id courts (type
//  "1.") et des suffixes courts ("pl","auto","etM"), on reste largement en
//  dessous même avec des id à deux chiffres.
//  La programmation horaire tient dans UNE seule clé par relais ("1.pl") :
//  le texte de toutes ses plages, au format canonique "HH:MM-HH:MM,HH:MM-
//  HH:MM" (voir plagesVersTexteBrut()/texteVersPlages()). Ajouter ou retirer
//  des plages ne change donc ni le nombre de clés, ni le nombre de champs.
//  ⚠️ On distingue "clé jamais enregistrée" (première mise en route : on
//  garde alors les valeurs par défaut de plagesDefaut) de "clé enregistrée
//  avec une liste vide" (l'utilisateur a volontairement tout effacé depuis
//  la page web) en utilisant NVS_PL_ABSENT comme valeur de secours : un texte
//  qu'aucune plage réelle ne peut jamais produire.
const char* NVS_PL_ABSENT = "\x01";

// saveSettings() : écrit l'état actuel de tous les programmateurs dans la
// mémoire NVS, afin de le retrouver après un redémarrage ou une coupure de courant.
void saveSettings() {
  Verrou v; 
  preferences.begin("config", false); // Ouvre le namespace "config" en lecture/écriture (false = read-write)

  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    String id = programmateurs[i].id;
    preferences.putString((id + "pl").c_str(), plagesVersTexteBrut(programmateurs[i].plages)); // plages en texte
    preferences.putBool((id + "auto").c_str(), programmateurs[i].modeAuto);
    preferences.putBool((id + "etM").c_str(), programmateurs[i].relayState);
  }

  preferences.end(); // Referme le namespace (valide et libère l'accès à la NVS)
  Serial.println("Paramètres sauvegardés dans la mémoire NVS");
}

// saveRelaySettings(p) : version CIBLÉE de saveSettings(), qui n'écrit en NVS
// que les 4 clés du relais concerné, au lieu de réécrire les clés des
// NB_PROGRAMMATEURS relais à chaque fois. 💾 Utile pour limiter l'utilisation de la
// mémoire flash (nombre de cycles d'écriture/effacement limité) : un simple
// appui sur un bouton poussoir ou un forçage ON/OFF n'a besoin de toucher
// qu'un seul relais, pas tout le tableau.
// Utilisée à la place de saveSettings() par /set-mode, /set-state, /save
// et checkPhysicalButtons() — c'est-à-dire partout où UN SEUL relais change.
// saveSettings() reste disponible telle quelle si un jour une sauvegarde
// complète de tous les relais est nécessaire.
void saveRelaySettings(const Programmateur &p) {
  Verrou v; 
  preferences.begin("config", false);
  String id = p.id;
  preferences.putString((id + "pl").c_str(), plagesVersTexteBrut(p.plages)); // plages en texte
  preferences.putBool((id + "auto").c_str(), p.modeAuto);
  preferences.putBool((id + "etM").c_str(), p.relayState);
  preferences.end();
  Serial.printf("Paramètres de %s sauvegardés dans la mémoire NVS\n", p.id);
}

// saveNoms(p) : enregistre le nom et le sous-titre saisis sur la page
// web. Un nom identique à celui du tableau programmateurs[] n'est pas stocké
// (clé effacée) : changer le nom par défaut dans le code reste ainsi possible.
void saveNoms(const Programmateur &p) {
  Verrou v;
  preferences.begin("config", false);
  String id = p.id;
  if (p.nomAff == p.nom) preferences.remove((id + "nm").c_str());
  else preferences.putString((id + "nm").c_str(), p.nomAff);
  if (p.sousNomAff == p.sousNom) preferences.remove((id + "sn").c_str());
  else preferences.putString((id + "sn").c_str(), p.sousNomAff);
  preferences.end();
  Serial.printf("Noms de %s sauvegardés : %s / %s\n", p.id, p.nomAff.c_str(), p.sousNomAff.c_str());
}

// 🔆 sauvegarderOled() : enregistre les réglages de luminosité de l'écran OLED.
// Nouvelles clés NVS indépendantes : CONFIG_VERSION n'a pas besoin de changer
// (au 1er démarrage de cette version, les valeurs par défaut sont utilisées).
void sauvegarderOled() {
  Verrou v;
  preferences.begin("config", false);
  preferences.putUChar("olJ", oledLumJour);
  preferences.putUChar("olN", oledLumNuit);
  preferences.putBool("olNA", oledNuitActive);
  preferences.putShort("olND", oledNuitDebut);
  preferences.putShort("olNF", oledNuitFin);
  preferences.end();
  Serial.printf("Luminosite OLED sauvegardee : jour %u%%, nuit %u%% (%s %s-%s)\n",
                oledLumJour, oledLumNuit, oledNuitActive ? "active" : "inactive",
                minutesEnHm(oledNuitDebut).c_str(), minutesEnHm(oledNuitFin).c_str());
}

// loadSettings() : relit la mémoire NVS au démarrage pour restaurer les
// horaires, modes et états précédemment sauvegardés. Si une clé n'existe pas
// encore (premier démarrage, ou nouveau relais ajouté au tableau), la valeur
// par défaut définie dans programmateurs[] est conservée automatiquement.
void loadSettings() {
  //  on ne compare plus FIRMWARE_BUILD pour décider d'effacer
  // la NVS. __DATE__/__TIME__ changent à chaque compilation et cette ancienne
  // méthode supprimait donc les réglages à chaque OTA, même après une simple
  // modification du programme, du HTML ou du CSS.
  // Ouverture en lecture/écriture (false) car on doit pouvoir créer ou mettre
  // à jour les informations de version et de diagnostic ci-dessous.
  preferences.begin("config", false);

  // Version de structure : elle ne change que lorsque la structure des
  // données NVS devient incompatible avec le programme. Une simple nouvelle
  // compilation ne provoque donc aucune perte de programmation utilisateur.
  uint16_t storedConfigVersion = preferences.getUShort("cfgVer", 0);

  // La signature du dernier firmware est conservée séparément uniquement
  // pour le diagnostic affiché dans la fenêtre "Infos système".
  String storedBuild = preferences.getString("lastBuild", "");
  buildPrecedentNVS = storedBuild;

  if (storedConfigVersion == 0) {
    // Première utilisation de cette gestion de version : on ne détruit PAS
    // les anciennes données NVS, car leur structure actuelle reste compatible.
    // On ajoute simplement la version pour les prochains démarrages.
    preferences.putUShort("cfgVer", CONFIG_VERSION);
    Serial.printf("Version NVS initialisee : %u (reglages existants conserves)\n", CONFIG_VERSION);
  } else if (storedConfigVersion != CONFIG_VERSION) {
    // Si la structure change réellement, les anciennes clés peuvent ne plus
    // être interprétables : dans ce cas seulement, on réinitialise la config.
    nvsReinitialiseeAuDemarrage = true;
    Serial.printf("Version NVS incompatible (%u -> %u) : reinitialisation de la configuration\n",
                  storedConfigVersion, CONFIG_VERSION);
    preferences.clear();                              // Efface le namespace "config"
    preferences.putUShort("cfgVer", CONFIG_VERSION); // Mémorise la nouvelle version
  }

  // Mémorise la nouvelle signature uniquement pour le diagnostic OTA.
  // Elle ne sert plus jamais de déclencheur d'effacement des horaires.
  if (storedBuild != String(FIRMWARE_BUILD)) {
    preferences.putString("lastBuild", FIRMWARE_BUILD);
  }

  preferences.end();

  preferences.begin("config", true); // Ouvre le namespace "config" en lecture seule (true = read-only)

  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    String id = programmateurs[i].id;

    // Plages : si la clé n'existe pas encore (premier démarrage, relais
    // ajouté au tableau, ou NVS réellement réinitialisée après changement de
    // CONFIG_VERSION), la valeur de secours NVS_PL_ABSENT est renvoyée telle
    // quelle et on conserve les plages construites à partir de plagesDefaut
    // par initPlagesDefaut(). Sinon (y compris une chaîne vide = l'utilisateur
    // a volontairement tout effacé), on applique le texte lu, même vide.
    String txt = preferences.getString((id + "pl").c_str(), NVS_PL_ABSENT);
    if (txt != NVS_PL_ABSENT) {
      texteVersPlages(txt.c_str(), programmateurs[i].plages);
    }

    // Restaure le mode automatique/manuelle sauvegardé pour ce relais.
    programmateurs[i].modeAuto = preferences.getBool((id + "auto").c_str(), programmateurs[i].modeAuto);

    // Restaure l'état mémorisé du relais. En mode automatique, cet état sera
    // recalculé dès que l'heure NTP sera disponible par appliquerProgrammation().
    programmateurs[i].relayState = preferences.getBool((id + "etM").c_str(), programmateurs[i].relayState);

    // Noms saisis sur la page web (sinon : noms par défaut du tableau)
    programmateurs[i].nomAff    = preferences.getString((id + "nm").c_str(), programmateurs[i].nom);
    programmateurs[i].sousNomAff = preferences.getString((id + "sn").c_str(), programmateurs[i].sousNom);
  }

  // 🔆 Luminosité de l'écran OLED (valeurs par défaut si jamais enregistrée)
  oledLumJour    = constrain(preferences.getUChar("olJ", oledLumJour), 1, 100);
  oledLumNuit    = constrain(preferences.getUChar("olN", oledLumNuit), 0, 100);
  oledNuitActive = preferences.getBool("olNA", oledNuitActive);
  oledNuitDebut  = constrain(preferences.getShort("olND", oledNuitDebut), 0, 1439);
  oledNuitFin    = constrain(preferences.getShort("olNF", oledNuitFin), 0, 1439);

  preferences.end(); // Referme le namespace
  Serial.println("Paramètres chargés depuis la mémoire NVS");
}

// ============================================================================
//  RECHERCHE ET CONNEXION AU MEILLEUR RÉSEAU WIFI CONNU
// ============================================================================
//  Scanne tous les réseaux WiFi visibles, ne garde que ceux qui figurent
//  dans knownNetworks[] (donc ceux dont on a le mot de passe dans arduino_secrets.h),
//  et se connecte à celui qui a le meilleur signal (RSSI le plus proche de 0).
//  Renvoie true si la connexion a réussi, false sinon.
// Historique des échecs par réseau connu (indexé comme knownNetworks[]) :
// évite de retenter en boucle un réseau qui a le meilleur signal mais qui
// vient d'échouer (ex : box en panne/plantée mais qui continue quand même
// d'émettre son SSID). Le réseau est mis de côté pendant BLACKLIST_DURATION
// avant d'être de nouveau considéré comme candidat.
// 🚩4️⃣  Commutation Réseau
unsigned long lastFailTime[knownNetworksCount] = {0};
const unsigned long BLACKLIST_DURATION = 30000; // 30 s d'exclusion après un échec

// ----------------------------------------------------------------------------
//  🚩4️⃣ Commutation Réseau — VERSION NON BLOQUANTE
// ----------------------------------------------------------------------------
//  Le scan WiFi (WiFi.scanNetworks()) et l'attente de connexion (WiFi.begin()
//  puis boucle jusqu'à WL_CONNECTED) sont par défaut des opérations
//  BLOQUANTES de plusieurs secondes : tant qu'elles durent, tout le reste de
//  loop() est à l'arrêt (boutons poussoirs, écran OLED, réponses du serveur
//  web...). On utilise ici le mode de scan ASYNCHRONE de l'ESP32
//  (WiFi.scanNetworks(true)) : le scan démarre en tâche de fond et son
//  résultat est récupéré plus tard via WiFi.scanComplete(), sans jamais
//  attendre. La recherche + connexion est donc découpée en une petite
//  machine à états (WifiConnStep), avancée d'un cran à chaque appel de
//  pollBestNetworkConnect() — à appeler régulièrement (voir loop()) — au
//  lieu d'être exécutée d'un bloc du début à la fin.
//  (types WifiConnStep / WifiConnResult déclarés tout en haut du fichier,
//  voir la remarque juste après "struct WifiNetwork")
WifiConnStep wifiConnStep = WCS_IDLE;
BetterNetStep betterNetStep = BNS_IDLE; // déplacée ici (utilisée aussi par startBestNetworkConnect)
int wifiConnTargetIndex = -1;        // Index (dans knownNetworks[]) du réseau auquel on tente de se connecter
unsigned long wifiConnStepStart = 0; // Instant (millis) du début de l'étape en cours (pour les timeouts)

// startBestNetworkConnect() : DÉMARRE une tentative de connexion (scan puis
// connexion au meilleur réseau connu trouvé). Ne bloque pas : revient
// immédiatement ; il faut ensuite appeler pollBestNetworkConnect() à chaque
// passage de loop() jusqu'à obtenir WCR_CONNECTED ou WCR_FAILED.
void startBestNetworkConnect() {
  // Force une déconnexion propre AVANT le scan. Indispensable : l'ESP32 a une
  // reconnexion automatique interne (activée par défaut) qui, si on ne la
  // coupe pas, continue de s'acharner en tâche de fond sur le dernier réseau
  // utilisé (même s'il ne répond plus) et entre en conflit avec le scan.
  // 🛟 Si le réseau de secours est ouvert, on ne coupe PAS la radio WiFi
  // (disconnect(true) éteindrait la partie "station") : on se contente de
  // couper la connexion en cours, pour ne pas gêner le smartphone connecté.
  //  TOUJOURS disconnect(false). Avant : disconnect(true) quand le
  // secours était fermé, ce qui ÉTEIGNAIT COMPLÈTEMENT la radio WiFi (mode
  // WIFI_OFF) puis le scan suivant la rallumait. Cet arrêt/redémarrage du
  // pilote WiFi sous le serveur web, le mDNS et le DNS, répété à chaque
  // coupure de box, ralentissait les cycles suivants et pouvait faire
  // planter l'ESP32 (redémarrage) au moment d'ouvrir le réseau de secours.
  WiFi.disconnect(false);
  Serial.printf("Recherche box : debut du scan (%s)\n", apSecoursActif ? "court" : "normal"); 
  // true = scan ASYNCHRONE : démarre en tâche de fond, ne bloque pas.
  // scan court pendant le secours (voir SCAN_MS_PAR_CANAL_SECOURS)
  WiFi.scanNetworks(true, false, false, apSecoursActif ? SCAN_MS_PAR_CANAL_SECOURS : 300);
  wifiConnStep = WCS_SCANNING;
  wifiConnStepStart = millis();
}

// pollBestNetworkConnect() : fait avancer d'UN CRAN, sans bloquer, une
// tentative démarrée par startBestNetworkConnect(). À appeler régulièrement
// (chaque passage de loop(), ou au moins chaque seconde) tant qu'elle est en
// cours (wifiConnStep != WCS_IDLE). Renvoie l'état courant.
WifiConnResult pollBestNetworkConnect() {
  if (wifiConnStep == WCS_SCANNING) {
    int n = WiFi.scanComplete(); // -1 = en cours, -2 = échec, >=0 = nb de réseaux trouvés
    if (n == WIFI_SCAN_RUNNING) return WCR_PENDING; // Toujours en cours : on repassera

    if (n == WIFI_SCAN_FAILED) {
      wifiConnStep = WCS_IDLE;
      return WCR_FAILED;
    }

    Serial.printf("Recherche box : scan fini en %lu ms, %d reseau(x)\n", millis() - wifiConnStepStart, n); 
    int bestIndex = -1;   // Index (dans knownNetworks[]) du meilleur réseau connu trouvé
    int bestRSSI = -1000; // Meilleur RSSI trouvé jusqu'ici (plus proche de 0 = meilleure réception)
    int bestIndexListeNoire = -1, bestRSSIListeNoire = -1000; 

    for (int i = 0; i < n; i++) {
      String foundSSID = WiFi.SSID(i);
      int foundRSSI = WiFi.RSSI(i);
      Serial.printf("  - %s (%d dBm)\n", foundSSID.c_str(), foundRSSI);

      // Ce réseau détecté fait-il partie de nos réseaux connus ?
      for (int k = 0; k < knownNetworksCount; k++) {
        // On ignore un réseau récemment en échec (mis en liste noire
        // temporaire), même si son signal est le meilleur : mieux vaut se
        // connecter à une box un peu moins bien captée mais qui fonctionne
        // réellement, plutôt que de retenter sans fin une box en panne.
        bool blacklisted = (lastFailTime[k] != 0 && millis() - lastFailTime[k] < BLACKLIST_DURATION);
        if (foundSSID == knownNetworks[k].ssid && foundRSSI > bestRSSI && !blacklisted) {
          bestRSSI = foundRSSI;
          bestIndex = k;
        }
        //  mémorise aussi le meilleur réseau connu EN LISTE NOIRE
        if (foundSSID == knownNetworks[k].ssid && blacklisted && foundRSSI > bestRSSIListeNoire) {
          bestRSSIListeNoire = foundRSSI;
          bestIndexListeNoire = k;
        }
      }
    }
    WiFi.scanDelete(); // Libère la mémoire utilisée par les résultats du scan

    //  si le SEUL réseau connu visible est en liste noire (cas typique :
    // la box vient de redémarrer et le 1er essai a échoué car elle n'était pas
    // encore prête), on le retente quand même plutôt que de rester en secours.
    if (bestIndex == -1 && bestIndexListeNoire != -1) {
      Serial.println("Seul reseau connu visible en liste noire : nouvel essai quand meme");
      bestIndex = bestIndexListeNoire;
      bestRSSI = bestRSSIListeNoire;
    }

    if (bestIndex == -1) {
      Serial.printf("Recherche box : aucun reseau connu visible\n"); 
      wifiConnStep = WCS_IDLE;
      return WCR_FAILED;
    }

    Serial.printf("Connexion a '%s' (%d dBm)...\n", knownNetworks[bestIndex].ssid, bestRSSI); 
    WiFi.begin(knownNetworks[bestIndex].ssid, knownNetworks[bestIndex].pass);
    wifiConnTargetIndex = bestIndex;
    wifiConnStepStart = millis();
    wifiConnStep = WCS_CONNECTING;
    return WCR_PENDING;
  }

  if (wifiConnStep == WCS_CONNECTING) {
    if (boxConnectee()) { 
      lastFailTime[wifiConnTargetIndex] = 0; // Ce réseau fonctionne : on efface un éventuel historique d'échec
      wifiConnStep = WCS_IDLE;
      return WCR_CONNECTED;
    }

    // Délai maximum de 15 s pour ne pas rester bloqué indéfiniment sur ce réseau
    if (millis() - wifiConnStepStart >= 15000) {
      // Échec : on marque ce réseau comme temporairement suspect pour laisser
      // sa chance à un autre réseau connu au prochain essai (voir loop()).
      lastFailTime[wifiConnTargetIndex] = millis();
      Serial.printf("Connexion a '%s' : echec apres 15 s\n", knownNetworks[wifiConnTargetIndex].ssid); 
      wifiConnStep = WCS_IDLE;
      return WCR_FAILED;
    }
    return WCR_PENDING; // Toujours en cours de connexion, on repassera
  }

  return WCR_PENDING; // wifiConnStep == WCS_IDLE : rien en cours
}

// connectToBestNetwork() : version BLOQUANTE, gardée UNIQUEMENT pour le
// démarrage (setup()), où il n'y a de toute façon rien d'autre à faire tant
// que le WiFi n'est pas up — bloquer ici est donc sans conséquence. Elle
// s'appuie en interne sur les mêmes fonctions non bloquantes que loop() (pas
// de logique dupliquée) : elle démarre juste une tentative puis attend son
// résultat en la faisant avancer en boucle.
bool connectToBestNetwork() {
  startBestNetworkConnect();
  WifiConnResult result;
  do {
    delay(50); // Petite pause pour ne pas monopoliser le CPU en boucle serrée
    result = pollBestNetworkConnect();
  } while (result == WCR_PENDING);
  return result == WCR_CONNECTED;
}

// ============================================================================
//  RETOUR AUTOMATIQUE VERS LE MEILLEUR RÉSEAU QUAND IL REDEVIENT DISPONIBLE
// ============================================================================
//  connectToBestNetwork() n'est appelée que lorsqu'on est déconnecté : une
//  fois repliés sur un second réseau connu, on y restait donc pour toujours,
//  même si le réseau habituellement le plus fort redevenait disponible.
//  Cette fonction est appelée périodiquement (voir loop()) MÊME quand on est
//  déjà connecté, pour vérifier si un réseau connu avec un signal nettement
//  meilleur est réapparu, et basculer dessus si c'est le cas.
// 🚩4️⃣  Commutation Réseau
const unsigned long BEST_NETWORK_RECHECK_INTERVAL = 60000; // 60 s entre deux vérifications
const int RSSI_SWITCH_MARGIN = 8; // Marge (en dB) exigée avant de basculer, pour éviter les allers-retours (hystérésis)

// checkForBetterNetwork() : version NON BLOQUANTE. Gère elle-même son
// minuteur interne (plus besoin de le faire au point d'appel, voir loop()) :
// tant qu'aucune vérification n'est en cours, elle ne fait rien avant
// BEST_NETWORK_RECHECK_INTERVAL ; une fois ce délai écoulé, elle lance un
// scan ASYNCHRONE (WiFi.scanNetworks(true)) et revient immédiatement. Les
// appels suivants se contentent de vérifier si le résultat est prêt
// (WiFi.scanComplete()) jusqu'à ce qu'il le soit — sans jamais bloquer
// loop(). À appeler à chaque passage de loop() (ou au moins chaque seconde).
// (type BetterNetStep déclaré tout en haut du fichier, même raison que
//  WifiConnStep/WifiConnResult — voir la remarque après "struct WifiNetwork")
// (betterNetStep : déclarée plus haut depuis la V3.7, à côté de wifiConnStep)

void checkForBetterNetwork() {
  static unsigned long lastCheckTime = 0;

  if (!boxConnectee()) { betterNetStep = BNS_IDLE; return; } // Rien à faire si on n'est pas connecté (c'est déjà géré ailleurs)

  if (betterNetStep == BNS_IDLE) {
    if (wifiConnStep != WCS_IDLE) return; // une tentative de connexion est déjà en cours
    if (millis() - lastCheckTime < BEST_NETWORK_RECHECK_INTERVAL) return; // Pas encore l'heure de revérifier
    lastCheckTime = millis();
    // Scan "à chaud" ASYNCHRONE (sans se déconnecter au préalable, contrairement
    // à startBestNetworkConnect()) : l'ESP32 gère seul la brève interruption que
    // cela implique, la connexion en cours n'est pas perdue.
    WiFi.scanNetworks(true);
    Serial.printf("Verif. meilleur reseau : scan\n"); 
    betterNetStep = BNS_SCANNING;
    return;
  }

  // BNS_SCANNING : un scan est en cours, on vérifie s'il est terminé
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return; // Toujours en cours, on repassera au prochain tick
  betterNetStep = BNS_IDLE;
  if (n == WIFI_SCAN_FAILED) return;

  String currentSSID = WiFi.SSID();
  int currentRSSI = WiFi.RSSI();
  int bestIndex = -1;
  int bestRSSI = -1000;

  for (int i = 0; i < n; i++) {
    String foundSSID = WiFi.SSID(i);
    int foundRSSI = WiFi.RSSI(i);
    for (int k = 0; k < knownNetworksCount; k++) {
      bool blacklisted = (lastFailTime[k] != 0 && millis() - lastFailTime[k] < BLACKLIST_DURATION);
      if (foundSSID == knownNetworks[k].ssid && foundRSSI > bestRSSI && !blacklisted) {
        bestRSSI = foundRSSI;
        bestIndex = k;
      }
    }
  }
  WiFi.scanDelete();

  // On ne bascule que si un AUTRE réseau connu a un signal nettement
  // meilleur (marge d'hystérésis) que le réseau actuel, pour éviter de
  // changer de réseau pour un écart de signal minime ou fluctuant.
  if (bestIndex != -1 &&
      String(knownNetworks[bestIndex].ssid) != currentSSID &&
      bestRSSI > currentRSSI + RSSI_SWITCH_MARGIN) {
    Serial.printf("Reseau '%s' (%d dBm) nettement meilleur que '%s' (%d dBm) : bascule...\n",
                  knownNetworks[bestIndex].ssid, bestRSSI, currentSSID.c_str(), currentRSSI);
    startBestNetworkConnect(); // Relance une connexion (non bloquante) vers ce meilleur réseau
  }
}

// ============================================================================
//  🛟 GESTION DU RÉSEAU WIFI DE SECOURS "ESP32_Secours"
// ============================================================================
//  L'ESP32 peut être en même temps client de la box (mode "station") ET
//  point d'accès pour le smartphone (mode "AP") : WiFi.softAP() ajoute le
//  point d'accès sans couper la partie station, qui continue de chercher la
//  box en tâche de fond. Le serveur web répond sur les deux réseaux à la fois.
//  "ESP_XXXXXX" : si la configuration du point d'accès est
// refusée par l'ESP32, il active quand même son point d'accès avec ses
// réglages USINE : un réseau nommé "ESP_" + fin de l'adresse MAC (ex:
// ESP_1A8241), SANS MOT DE PASSE. Deux causes fréquentes :
//   1) mot de passe de moins de 8 caractères (refusé par le WPA2) ;
//   2) configuration envoyée pendant un scan WiFi en cours (recherche de la box).
// On évite donc d'ouvrir le réseau pendant un scan, on vérifie le mot de
// passe AVANT, et on contrôle APRÈS coup que le nom diffusé est bien le bon :
// sinon on referme immédiatement ce réseau ouvert et on réessaie plus tard.
void demarrerReseauSecours() {
  if (apSecoursActif) return;

  // Cause 1 : mot de passe trop court -> on n'ouvre rien (plutôt qu'un réseau ouvert)
  if (strlen(AP_PASS) < 8) {
    static bool dejaSignale = false;
    if (!dejaSignale) {
      Serial.println("ERREUR : SECRET_AP_PASS doit faire au moins 8 caracteres - reseau de secours NON ouvert");
      dejaSignale = true;
    }
    return;
  }

  // Cause 2 : un scan WiFi est en cours -> on attend qu'il soit terminé
  // (gererReseauSecours() nous rappellera à la seconde suivante).
  if (wifiConnStep == WCS_SCANNING || betterNetStep == BNS_SCANNING ||
      WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    Serial.printf("Secours : ouverture differee (scan en cours)\n"); 
    return;
  }

  Serial.printf("Secours : ouverture...\n"); 
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  delay(100); // Laisse le temps au point d'accès de s'initialiser
  // 🛟 🚩Pour changer l'adresse IP du mode Secours
    WiFi.softAPConfig(IPAddress(192,168,5,1), IPAddress(192,168,5,1), IPAddress(255,255,255,0)); // adresse du réseau de secours
//                         Adresse IP     Passerelle (identique Adresse IP)    Masque de sous réseau
  // Vérification : le nom réellement diffusé doit être "ESP32_Secours"
  if (ok && WiFi.softAPSSID() == String(AP_SSID)) {
    apSecoursActif = true;
    //  tous les noms de domaine -> 192.168.5.1 (portail captif)
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    Serial.printf("Secours : OUVERT http://%s\n", WiFi.softAPIP().toString().c_str()); 
  } else {
    Serial.printf("Secours : ECHEC ouverture (nom '%s')\n", WiFi.softAPSSID().c_str()); 
    WiFi.softAPdisconnect(true); // Ferme le réseau "ESP_XXXXXX" ouvert par défaut, sans mot de passe
  }
}

void arreterReseauSecours() {
  if (!apSecoursActif) return;
  dnsServer.stop(); 
  WiFi.softAPdisconnect(true); // Ferme le point d'accès (la connexion à la box n'est pas touchée)
  apSecoursActif = false;
  Serial.printf("Secours : FERME (box retrouvee)\n"); 
}

// À appeler chaque seconde (voir loop()) : ouvre le réseau de secours quand
// la box est perdue depuis AP_DELAI_ACTIVATION, le referme quand elle est
// revenue depuis AP_DELAI_DESACTIVATION (même si un smartphone y est encore
// connecté quand AP_FERMETURE_MEME_SI_CONNECTE = true : il rebascule sur la box).
void gererReseauSecours() {
  // (boxPerdueDepuis : variable globale depuis la V3.7, voir plus haut)
  static unsigned long boxRetrouveeDepuis = 0; // 0 = box actuellement perdue

  //  Sécurité : un point d'accès actif alors qu'on ne l'a pas ouvert
  // nous-mêmes ne peut être que le réseau "ESP_XXXXXX" par défaut, SANS mot
  // de passe : on le ferme aussitôt.
  if (!apSecoursActif && (WiFi.getMode() & WIFI_AP)) {
    Serial.printf("Point d'acces inattendu '%s' : fermeture\n", WiFi.softAPSSID().c_str()); 
    WiFi.softAPdisconnect(true);
  }

  if (AP_SECOURS_TOUJOURS_ACTIF) { demarrerReseauSecours(); return; }

  if (!boxConnectee()) { 
    boxRetrouveeDepuis = 0;
    if (boxPerdueDepuis == 0) boxPerdueDepuis = millis();
    if (!apSecoursActif && millis() - boxPerdueDepuis >= AP_DELAI_ACTIVATION) {
      demarrerReseauSecours();
    }
  } else {
    boxPerdueDepuis = 0;
    if (apSecoursActif) {
      if (boxRetrouveeDepuis == 0) boxRetrouveeDepuis = millis();
      bool personne = (WiFi.softAPgetStationNum() == 0);
      if (millis() - boxRetrouveeDepuis >= AP_DELAI_DESACTIVATION &&
          (personne || AP_FERMETURE_MEME_SI_CONNECTE)) {
        if (!personne) Serial.println("Box retrouvee : fermeture du secours, le smartphone va rebasculer sur la box");
        arreterReseauSecours();
      }
    }
  }
}

// ============================================================================
//  CONVERSION DE LA PUISSANCE DU SIGNAL WIFI (dBm) EN POURCENTAGE
// ============================================================================
//  Le RSSI (Received Signal Strength Indicator) fourni par WiFi.RSSI() est
//  exprimé en dBm, une échelle logarithmique négative (ex: -45 dBm = très bon
//  signal, -90 dBm = signal très faible), peu parlante pour un utilisateur.
//  On le convertit ici en pourcentage (0-100%) grâce au barème habituellement
//  utilisé par les systèmes d'exploitation (Windows, Android...) : -50 dBm ou
//  mieux = 100%, -100 dBm ou pire = 0%, et une relation linéaire entre les deux.
int rssiToPercent(int rssiDbm) {
  if (rssiDbm <= -100) return 0;
  if (rssiDbm >= -50) return 100;
  return 2 * (rssiDbm + 100); // Ex: -70 dBm -> 2*(30) = 60%
}

// ============================================================================
//  MISE À JOUR DE L'ÉCRAN OLED (adresse I2C 0x3C)
// ============================================================================
//  Affiche le réseau WiFi utilisé, l'adresse IP de l'ESP32, et pour chaque
//  relais : son mode (A=Auto / M=Manuel), son état ON/OFF — plus de texte
//  "ON"/"OFF" : c'est l'ID du relais lui-même qui passe en vidéo inverse
//  quand il est actif (voir printRelayLine()), ce qui libère de la place —
//  et (uniquement en mode automatique) sa plage active + sa plage à venir,
//  ex: "08:00>11:30-13:15" (s'éteint à 08:00, prochaine plage 11:30-13:15)
//  ou "11:30-13:15" (pas encore actif : prochaine plage en entier).
//  L'écran étant petit (64 px de haut, ~5 lignes de relais visibles après
//  l'en-tête), l'affichage passe automatiquement en PAGES tournantes dès que
//  le nombre de relais dépasse OLED_LIGNES_PAR_PAGE : chaque page reste
//  affichée 8 secondes avant de passer à la suivante (voir loop()).

// Affiche une ligne d'état pour un relais donné.
// Plus de texte "ON"/"OFF" : l'état du relais est indiqué en mettant
// directement l'ID en vidéo inverse (fond blanc, texte noir) quand il est
// actif — comme le faisait déjà "ON" avant, mais sans les 3 caractères
// "ON "/"OFF " que ce mot occupait. Préfixe réduit à "id + mode" (4
// caractères, ex: "1.A "), ce qui laisse jusqu'à 17 caractères pour la
// plage active + la plage à venir qui suivent (voir plageActiveEtSuivante()) :
// l'écran ne fait que 21 caractères de large.
//  "mode" vaut 'A' (Auto), 'M' (Manuel) ou 'F' (Forçage temporaire).
void printRelayLine(const char* name, char mode, bool relayState,
                     const String &resume) {
  int16_t x = display.getCursorX();
  int16_t y = display.getCursorY();
  int largeurId = 6 * strlen(name); // Police GFX par défaut : 6 px/caractère en textSize 1

  if (relayState) {
    // Relais actif : l'ID est affiché en vidéo inverse (fond blanc, texte noir)
    display.fillRect(x, y, largeurId, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  }
  display.print(name);
  display.setTextColor(SSD1306_WHITE); // Remet la couleur normale pour la suite (mode + horaires)
// 🔔🔕 M pour mode Manuel, A pour automatique, F pour forçage temporaire
  display.print(' ');
  display.print(mode);
  display.print(' ');

  if (mode != 'M') {
    // Plage active (heure de fin) + plage à venir en entier, ex:
    // "08:00>11:30-13:15" (voir le commentaire au-dessus de
    // plageActiveEtSuivante()), ou fin du forçage temporaire ("jusqu'a 14:32").
    display.println(resume);
  } else {
    display.println(); // Mode manuel : pas d'horaire de programmation à afficher
  }
}

// ============================================================================
//  EN-TÊTE DE L'ÉCRAN OLED : DATE + HEURE / BOX / ADRESSE IP, CENTRÉS
// ============================================================================
// Affiche un texte centré horizontalement sur la ligne courante, puis passe à
// la ligne suivante. Un texte trop long est coupé à OLED_CARS_PAR_LIGNE.
void oledLigneCentree(String txt) {
  if ((int)txt.length() > OLED_CARS_PAR_LIGNE) txt = txt.substring(0, OLED_CARS_PAR_LIGNE);
  int16_t y = display.getCursorY();
  display.setCursor((SCREEN_WIDTH - 6 * txt.length()) / 2, y);
  display.print(txt);
  display.setCursor(0, y + 8); // Ligne suivante (8 px de haut)
}

// Ligne date + heure : "Mardi 06 Octobre - 18:14".
// L'écran n'affiche que 21 caractères par ligne : selon la longueur du jour et
// du mois, le texte est raccourci par étapes jusqu'à ce qu'il tienne :
//   "Mardi 06 Octobre - 18:14"  (24 car.)  -> trop long
//   "Mardi 06 Octobre 18:14"    (22 car.)  -> trop long
//   "Mardi 06 Oct - 18:14"      (20 car.)  -> affiché
// (abréviations sans point, pour gagner de la place)
// Les accents (é, û) utilisent le jeu de caractères de l'écran (cp437, activé
// dans setup()) et comptent pour un seul caractère. "*" en fin de ligne =
// heure réglée à la main. t = nullptr -> heure inconnue.
String ligneDateHeure(const struct tm *t) {
  if (!t) return String("Heure non reglee");
  static const char* const JOURS[7]   = { "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche" };
  static const char* const JOURS_C[7] = { "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam", "Dim" };
  static const char* const MOIS[12]   = { "Janvier", "F\x82vrier", "Mars", "Avril", "Mai", "Juin", "Juillet",
                                          "Ao\x96t", "Septembre", "Octobre", "Novembre", "D\x82" "cembre" };
  static const char* const MOIS_C[12] = { "Janv", "F\x82v", "Mars", "Avr", "Mai", "Juin", "Juil",
                                          "Ao\x96t", "Sept", "Oct", "Nov", "D\x82" "c" };
  int j = (t->tm_wday + 6) % 7; // 0 = lundi
  char jj[3], hm[6];
  snprintf(jj, sizeof(jj), "%02d", t->tm_mday);
  snprintf(hm, sizeof(hm), "%02d:%02d", t->tm_hour, t->tm_min);
  String fin = String(hm) + (heureManuelle ? "*" : "");

  // Variantes de la plus complète à la plus courte : la première qui tient est retenue
  String variantes[5] = {
    String(JOURS[j])   + " " + jj + " " + MOIS[t->tm_mon]   + " - " + fin,
    String(JOURS[j])   + " " + jj + " " + MOIS[t->tm_mon]   + " "   + fin,
    String(JOURS[j])   + " " + jj + " " + MOIS_C[t->tm_mon] + " - " + fin,
    String(JOURS[j])   + " " + jj + " " + MOIS_C[t->tm_mon] + " "   + fin,
    String(JOURS_C[j]) + " " + jj + " " + MOIS_C[t->tm_mon] + " "   + fin,  // 18 car. max : tient toujours
  };
  for (int k = 0; k < 4; k++) {
    if ((int)variantes[k].length() <= OLED_CARS_PAR_LIGNE) return variantes[k];
  }
  return variantes[4];
}

// page : numéro de page à afficher (0 = les OLED_LIGNES_PAR_PAGE premiers
// relais, 1 = les suivants, etc.). Recyclé automatiquement (modulo) selon le
// nombre total de pages nécessaires.
void updateOLED(int page) {
  if (!oledOK) return; // Écran non détecté au démarrage : on ne fait rien

  int nbPages = (NB_PROGRAMMATEURS + OLED_LIGNES_PAR_PAGE - 1) / OLED_LIGNES_PAR_PAGE;
  if (nbPages < 1) nbPages = 1;
  page = page % nbPages;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  // Récupération de l'heure actuelle pour l'écran OLED
  struct tm timeinfo;
  int nowMinOled = -1; // Minute de la SEMAINE courante, pour plageActiveEtSuivante() (-1 = heure inconnue, pas encore de synchro NTP)
  bool heureOK = lireHeureLocale(&timeinfo);
  if (heureOK) nowMinOled = minuteSemaine(timeinfo);

  // En-tête sur 3 lignes, centrées :
  // Ligne 1 : date + heure ("Mardi 06 Oct - 18:14")
  oledLigneCentree(ligneDateHeure(heureOK ? &timeinfo : nullptr));

  // Ligne 2 : nom de la box (ou "AP  ESP32_Secours" sans box, réseau de secours ouvert)
  bool boxOK = boxConnectee(); 
  if (boxOK)               oledLigneCentree(WiFi.SSID());
  else if (apSecoursActif) oledLigneCentree("* " + String(AP_SSID));
  else                     oledLigneCentree("Box non connectee");

  // Ligne 3 : adresse IP (box ou réseau de secours), suivie du numéro de page
  // quand il y a plus de relais que de lignes disponibles : "192.168.1.42   P-1/2".
  // 3 espaces de séparation, réduits si l'adresse IP est longue, pour que la
  // ligne tienne toujours dans les 21 caractères de l'écran.
  String ip = boxOK ? WiFi.localIP().toString()
            : apSecoursActif ? WiFi.softAPIP().toString()
            : String("--");
  if (nbPages > 1) {
    String pg = "P-" + String(page + 1) + "/" + String(nbPages);
    int espaces = OLED_CARS_PAR_LIGNE - (int)ip.length() - (int)pg.length();
    espaces = constrain(espaces, 1, 3);
    for (int k = 0; k < espaces; k++) ip += " ";
    ip += pg;
  }
  oledLigneCentree(ip);

  // Lignes suivantes : un relais par ligne, pour la page courante uniquement
  int start = page * OLED_LIGNES_PAR_PAGE;
  int end = min(start + OLED_LIGNES_PAR_PAGE, NB_PROGRAMMATEURS);
  for (int i = start; i < end; i++) {
    // Plage active (heure de fin) + plage à venir en entier
    // ("08:00>11:30-13:15"), voir plageActiveEtSuivante().
    Programmateur &p = programmateurs[i];
    char mode = !p.modeAuto ? 'M' : (p.forcageActif ? 'F' : 'A');
    String resume;
    if (mode == 'F') {
      // Forçage temporaire : heure de retour en AUTO ("jusqu'a 14:32")
      long reste = (forcageRestantSec(p) + 59) / 60;
      if (nowMinOled >= 0) resume = "jusqu'a " + minutesEnHm((nowMinOled % 1440) + reste);
      else                 resume = "encore " + String(reste) + " min";
    } else {
      resume = plageActiveEtSuivante(p.plages, nowMinOled);
    }
    printRelayLine(p.id, mode, p.relayState, resume);
  }

  display.display();
}

// ============================================================================
//  🔆 APPLICATION DE LA LUMINOSITÉ OLED
// ============================================================================
// Envoie un niveau de luminosité (en %) à l'écran. 0 % = écran éteint.
// Le contraste seul (commande 0x81) a un effet limité sur un SSD1306 : pour
// les niveaux faibles, on réduit aussi la précharge (0xD9) et la tension
// VCOMH (0xDB), ce qui assombrit nettement plus l'affichage.
void reglerLuminositeOLED(uint8_t pct) {
  if (!oledOK) return;
  if (pct == 0) {
    display.ssd1306_command(SSD1306_DISPLAYOFF); // Écran noir (le contenu est conservé)
    return;
  }
  if (pct > 100) pct = 100;
  uint8_t contraste = (uint8_t)((pct * 255 + 50) / 100);
  bool faible = (pct < 25);
  display.ssd1306_command(SSD1306_SETCONTRAST);   // 0x81
  display.ssd1306_command(contraste);
  display.ssd1306_command(SSD1306_SETPRECHARGE);  // 0xD9
  display.ssd1306_command(faible ? 0x22 : 0xF1);  // 0xF1 = valeur d'origine (alim. interne)
  display.ssd1306_command(SSD1306_SETVCOMDETECT); // 0xDB
  display.ssd1306_command(faible ? 0x00 : 0x40);  // 0x40 = valeur d'origine
  display.ssd1306_command(SSD1306_DISPLAYON);
}

// L'heure courante est-elle dans la plage "nuit" ? (false si l'heure est inconnue)
bool estModeNuit() {
  if (!oledNuitActive || oledNuitDebut == oledNuitFin) return false;
  struct tm t;
  if (!lireHeureLocale(&t)) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  if (oledNuitDebut < oledNuitFin) return m >= oledNuitDebut && m < oledNuitFin;
  return m >= oledNuitDebut || m < oledNuitFin; // Plage à cheval sur minuit
}

// Niveau qui DEVRAIT être appliqué maintenant (aperçu > nuit > jour).
uint8_t luminositeVoulue() {
  if (oledApercuNiveau >= 0) {
    if (millis() - oledApercuDebut < OLED_APERCU_MS) return (uint8_t)oledApercuNiveau;
    oledApercuNiveau = -1; // Aperçu terminé
  }
  return estModeNuit() ? oledLumNuit : oledLumJour;
}

// Appelée chaque seconde par loop() : n'envoie une commande à l'écran que si
// le niveau voulu a changé (pas de trafic I2C inutile).
void gererLuminositeOLED() {
  int niveau = luminositeVoulue();
  if (niveau != oledNiveauApplique) {
    reglerLuminositeOLED(niveau);
    oledNiveauApplique = niveau;
  }
}

// ============================================================================
//  🔘 BOUTONS POUSSOIRS DE FORÇAGE PHYSIQUE (1. à 4. uniquement)
// ============================================================================
//  Câblage : GND --- Bouton Poussoir --- GPIO (broche déclarée dans "pinBP").
//  La broche est configurée en INPUT_PULLUP (voir setup()) : au repos elle
//  lit HIGH (tirée au +3.3V en interne), et lit LOW quand le bouton est
//  appuyé (il relie alors la broche au GND).
//
//  Cette fonction est appelée à CHAQUE passage de loop() (pas seulement une
//  fois par seconde) pour que l'appui soit détecté sans délai perceptible.
//  Elle applique un anti-rebond logiciel (debounce) : un changement de
//  lecture n'est pris en compte que s'il reste stable pendant BP_DEBOUNCE_MS.
//
//  Un appui détecté (passage stable à LOW) produit exactement le même effet
//  que le bouton "FORCER ON/OFF" de la page web : passage en mode manuel +
//  inversion de l'état du relais, puis sauvegarde en NVS.
void checkPhysicalButtons() {
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    Programmateur &p = programmateurs[i];
    if (p.pinBP < 0) continue; // Ce programmateur n'a pas de bouton poussoir câblé

    bool lecture = digitalRead(p.pinBP); // LOW = bouton appuyé (relié au GND)

    // Si la lecture brute vient de changer, on redémarre le chrono anti-rebond
    if (lecture != bpDernierEtatLu[i]) {
      bpDerniereBascule[i] = millis();
      bpDernierEtatLu[i] = lecture;
    }

    // La lecture est-elle stable depuis assez longtemps pour être validée ?
    if (millis() - bpDerniereBascule[i] > BP_DEBOUNCE_MS) {
      if (lecture != bpEtatStable[i]) {
        bpEtatStable[i] = lecture; // Nouvel état stable retenu

        if (bpEtatStable[i] == LOW) { // Front descendant = appui réellement détecté
          Verrou v;                       // la page web peut agir au même moment
          p.modeAuto = false;             // Passe en mode manuel
          p.forcageActif = false;         //  annule un éventuel forçage temporaire
          p.relayState = !p.relayState;   // Inverse l'état du relais (un bouton physique reste une bascule)
          ecrireRelais(p, p.relayState); // Applique immédiatement sur la sortie physique
          saveRelaySettings(p);            // 💾 Sauvegarde ciblée : seul ce relais est réécrit en NVS
          Serial.printf("Bouton poussoir %s (GPIO%d) : forcage manuel -> %s\n",
                        p.id, p.pinBP, p.relayState ? "ON" : "OFF");
        }
      }
    }
  }
}

// ============================================================================
//  🔄MISE À JOUR DU FIRMWARE PAR WIFI (OTA)
// ============================================================================
//  Permet de reflasher le programme depuis l'IDE Arduino SANS câble USB, une
//  fois l'ESP32 installé dans son emplacement définitif (ex: boîtier
//  électrique). Configurée dans setup() (voir setupOTA()) et servie à chaque
//  passage de loop() via ArduinoOTA.handle(). Après un premier flash par USB
//  avec l'OTA actif, la carte apparaît ensuite comme un "port réseau" dans
//  Outils > Port de l'IDE Arduino, tant qu'elle reste sur le même réseau WiFi.
void setupOTA() {
  ArduinoOTA.setHostname(hostname); // Même nom que le mDNS web (ex: "richardv")

  // 🔒 Mot de passe OTA (fortement recommandé) : sans lui, n'importe quel
  // appareil du réseau WiFi pourrait reflasher l'ESP32. Définissez
  // SECRET_OTA_PASSWORD dans arduino_secrets.h pour l'activer, par exemple :
  //   #define SECRET_OTA_PASSWORD "votre_mot_de_passe"
#ifdef SECRET_OTA_PASSWORD
  ArduinoOTA.setPassword(SECRET_OTA_PASSWORD); // 🔒 Mot de passe OTA défini dans arduino_secrets.h
#else
  Serial.println("ATTENTION : OTA sans mot de passe (definissez SECRET_OTA_PASSWORD dans arduino_secrets.h pour le securiser)");
#endif

  // 🖥️ Callbacks : affichent la progression sur l'écran OLED (si présent) et sur
  // le moniteur série pendant le transfert du nouveau firmware.
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "programme" : "systeme de fichiers";
    Serial.println("Debut de la mise a jour (" + type + ")...");

    //  OTA FIABILISÉE : on libère au maximum le WiFi et le processeur
    // pendant le transfert. Un scan WiFi (recherche de la box ou d'un
    // meilleur réseau) ou les requêtes de la page web ouverte sur un
    // smartphone pouvaient faire échouer la mise à jour : l'ancien programme
    // restait alors en place et il fallait téléverser une 2e fois.
    esp_wifi_scan_stop();   // arrête un éventuel scan WiFi en cours
    WiFi.scanDelete();
    server.end();           // coupe le serveur web (relancé par le redémarrage)
    if (apSecoursActif) dnsServer.stop();
    // 🔆 Pleine luminosité pendant la mise à jour (même si l'écran était
    // éteint ou atténué par le mode nuit) : la progression reste lisible.
    reglerLuminositeOLED(100);
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(5, 12);  // Décale de 4 caractères vers la droite et 2 lignes vers le bas
      display.print("   Mise a jour...");
      display.setCursor(5, 28);
      display.print("  Ne pas eteindre !");
      display.display();
      // ⏱️ Delai d'affichage du message avant que la barre de progression
      // ne prenne le relais.  réduit de 2 s à 0,5 s, car l'outil
      // OTA de l'IDE attend pendant ce temps que l'ESP32 le rappelle.
      delay(500);
    }
  });

  //******* 🔄Modes OTA
    ArduinoOTA.onEnd([]() {
    Serial.println("\nMise a jour terminee, redemarrage...");
        delay(2500); // Delais entre 100% et message Mise a jour OK
    const char* txtOk = "  Mise a jour OK...";    // --- Configuration des textes et calcul du centrage ---
    const char* txtRedem = "  Redemmarage... ";
    int16_t x1, y1;
    uint16_t largTexte, hautTexte;
    display.setTextSize(1);   // Calcul automatique de la position X pour centrer "Redemmarage..."
    display.getTextBounds(txtRedem, 0, 0, &x1, &y1, &largTexte, &hautTexte);
    int16_t posX_Redem = (SCREEN_WIDTH - largTexte) / 2; // Centrage horizontal au pixel près
    int16_t posY_Redem = 38;                             // Positionné environ 2 lignes en dessous de la ligne 12
    for (int i = 0; i < 4; i++) {  // --- Boucle de clignotement (5 cycles d'alternance) ---
      display.clearDisplay();   // ÉTAPE A : "Mise a jour OK..." + "Redemmarage..." en Normal (Blanc sur Noir)
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); // Ligne du haut (Fixe)
      display.setCursor(2, 12);
      display.print(txtOk);
      display.setCursor(posX_Redem, posY_Redem);    // Ligne du bas (Normal)
      display.print(txtRedem);
      display.display();
      delay(400); // Durée de l'état normal
      display.clearDisplay(); // ÉTAPE B : "Mise a jour OK..." + "Redemmarage..." en Vidéo Inversée (Noir sur Blanc)
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);// Ligne du haut (Reste fixe en Normal)
      display.setCursor(2, 12);
      display.print(txtOk);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);// Ligne du bas (Bascule en Vidéo Inversée)
      display.setCursor(posX_Redem, posY_Redem);
      display.print(txtRedem);
      display.display();
      delay(400); // Durée de l'état inversé
    }
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); // Remet la couleur par défaut pour la suite du programme
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    unsigned int pct = (total > 0) ? (progress * 100 / total) : 0;
    Serial.printf("Progression OTA : %u%%\r", pct);
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(12, 16);  // Décale de 2 caractères vers la droite et 2 lignes vers le bas
      display.printf("Mise a jour : %d%%", pct);
      display.drawRect(12, 32, 100, 10, 1); 
      //Dessine la barre de progression. La largeur du rectangle plein est égale à --> 'pct' (de 0 à 100 pixels)
      display.fillRect(12, 32, pct, 10, 1); 
      display.display();
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Erreur OTA [%u] : ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Authentification echouee");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Echec au demarrage");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Echec de connexion");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Echec de reception");
    else if (error == OTA_END_ERROR) Serial.println("Echec a la finalisation");

    //  échec visible sur l'OLED, puis redémarrage propre. L'ancien
    // programme reste en place (il n'est remplacé qu'en fin de transfert
    // réussi) : il suffit de relancer le téléversement depuis l'IDE.
    if (oledOK) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      display.setCursor(5, 12);
      display.print("  ECHEC mise a jour");
      display.setCursor(5, 30);
      display.print(" Ancien programme");
      display.setCursor(5, 42);
      display.print("  conserve. Refaire");
      display.setCursor(5, 54);
      display.print("  le televersement");
      display.display();
    }
    delay(4000);
    ESP.restart();   // relance serveur web, DNS, scans... sur l'ancien programme
  });

  ArduinoOTA.begin();
  Serial.println("OTA pret : mise a jour possible via le WiFi depuis l'IDE Arduino");
}

// ============================================================================
//   OUTILS POUR LES ROUTES WEB
// ============================================================================
// Lit un paramètre envoyé dans le corps d'un POST (formulaire), ou à défaut
// dans l'adresse (?id=...). Renvoie false s'il est absent.
bool lireParam(AsyncWebServerRequest *request, const char *nom, String &valeur) {
  if (request->hasParam(nom, true)) { valeur = request->getParam(nom, true)->value(); return true; }
  if (request->hasParam(nom))       { valeur = request->getParam(nom)->value();       return true; }
  return false;
}

// Nettoie un nom saisi sur la page web : espaces en trop et caractères de
// contrôle retirés, longueur limitée à MAX_LONGUEUR_NOM caractères (sans
// couper un caractère accentué, codé sur plusieurs octets en UTF-8).
String nettoyerNom(const String &brut) {
  String out;
  int nbCar = 0;
  for (int i = 0; i < (int)brut.length(); i++) {
    uint8_t c = (uint8_t)brut[i];
    if (c < 0x20 || c == 0x7F) continue;          // Caractère de contrôle : ignoré
    bool debutCaractere = (c & 0xC0) != 0x80;     // Octet de suite UTF-8 = même caractère
    if (debutCaractere && ++nbCar > MAX_LONGUEUR_NOM) break;
    out += (char)c;
  }
  out.trim();
  return out;
}

// ============================================================================
//  INITIALISATION (exécutée une seule fois au démarrage de l'ESP32)
// ============================================================================
void setup() {
  Serial.begin(115200);          // Démarre la liaison série (pour le moniteur série, débit 115200 bauds)

  // Affiche la signature de build en tout premier : c'est la preuve
  // (dans le moniteur série, et sur l'OLED via updateOLED()) qu'un nouveau
  // firmware a bien été flashé après une mise à jour OTA.
  Serial.printf("Cause du redemarrage : %s\n", raisonRedemarrage()); 
  Serial.print("Firmware compile le : ");
  Serial.println(FIRMWARE_BUILD);

  // Construit les plages de chaque relais à partir du texte écrit en clair
  // dans le tableau programmateurs[] (champ plagesDefaut). À faire
  // IMPÉRATIVEMENT avant loadSettings(), pour que les plages éventuellement
  // enregistrées en NVS reprennent ensuite le dessus.
  initPlagesDefaut();

  // On charge d'abord les réglages sauvegardés (plages, modes, états),
  // AVANT de toucher aux broches. On connaît ainsi l'état voulu de chaque
  // relais avant même de configurer sa broche en sortie.
  loadSettings();

  // Configure la broche de chaque relais déclaré dans programmateurs[] en
  // sortie numérique. Astuce anti-glitch : on appelle digitalWrite() AVANT
  // pinMode(OUTPUT). Sur l'ESP32/Arduino, cela pré-charge le registre de
  // sortie avec le bon niveau, de sorte que dès que la broche bascule en
  // sortie, elle prend directement l'état voulu — sans repasser un court
  // instant par LOW par défaut (ce qui, avec un module relais "actif à
  // l'état bas" comme la plupart des modules bon marché à base de
  // SRD-05VDC, active brièvement le relais avant qu'il ne retombe).
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    ecrireRelais(programmateurs[i], programmateurs[i].relayState);
    pinMode(programmateurs[i].pin, OUTPUT);
  }

  // 🔘 Configure la broche du bouton poussoir de forçage physique (si déclarée)
  // en entrée avec résistance de tirage interne au +3.3V (INPUT_PULLUP) : le
  // bouton se contente donc de relier la broche au GND, sans résistance externe.
  // Initialise aussi les tableaux d'anti-rebond correspondants.
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    bpDernierEtatLu[i] = HIGH;
    bpEtatStable[i] = HIGH;
    bpDerniereBascule[i] = 0;
    if (programmateurs[i].pinBP >= 0) {
      pinMode(programmateurs[i].pinBP, INPUT_PULLUP);
    }
  }

  // Remarque : contrairement à LittleFS, la bibliothèque Preferences ne
  // nécessite pas d'initialisation globale ici — chaque appel à
  // preferences.begin()/end() (dans saveSettings/loadSettings) gère seul
  // son accès à la mémoire NVS.

  // Initialise le bus I2C (broches par défaut ESP32 : SDA=21, SCL=22) et l'écran OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Ecran OLED non detecte a l'adresse 0x3C (verifier le cablage)");
    oledOK = false;
  } else {
    oledOK = true;
    display.cp437(true); // Vrai jeu de caractères de l'écran (accents é, û de la date)
    gererLuminositeOLED(); // 🔆 Luminosité enregistrée (heure encore inconnue -> niveau "jour")
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Demarrage...");
    display.display();
  }

  // (loadSettings() et l'application de l'état des relais ont été déplacés
  // plus haut, avant la configuration des broches — voir remarque ci-dessus)
  // Connexion WiFi : scanne les réseaux disponibles et se connecte à celui,
  // parmi les réseaux connus (arduino_secrets.h), qui offre le meilleur signal.
  WiFi.mode(WIFI_STA);          // Mode "station" (client), nécessaire avant le scan
  WiFi.setHostname(hostname);   // Nom affiché côté routeur/box
  // Désactive la reconnexion automatique interne de l'ESP32 : c'est notre
  // fonction connectToBestNetwork() qui doit seule décider à quel réseau se
  // connecter (sinon l'ESP32 s'acharne en interne sur le dernier réseau
  // utilisé même s'il ne répond plus, ce qui empêchait le repli automatique
  // vers un autre réseau connu).
  // 🚩4️⃣  Commutation Réseau
  WiFi.setAutoReconnect(false);

  // la box est déclarée perdue après 3 s sans balise WiFi reçue
  // (6 s par défaut ; 3 s est le minimum accepté par l'ESP32).
  esp_err_t errInactif = esp_wifi_set_inactive_time(WIFI_IF_STA, 3);
  Serial.printf("Detection perte box : %s\n", errInactif == ESP_OK ? "3 s" : "defaut");

  // 🛟 Avant (V2) : sans box au démarrage, l'ESP32 redémarrait en boucle
  // (ESP.restart()) et devenait totalement inaccessible pendant une coupure.
  // Désormais il ouvre immédiatement le réseau de secours et continue de
  // fonctionner normalement (relais, boutons, page web) ; la box sera
  // recherchée en tâche de fond par loop().
  bool boxAuDemarrage = connectToBestNetwork();
  if (!boxAuDemarrage) {
    Serial.println("Box injoignable : ouverture du reseau de secours");
    demarrerReseauSecours();
    if (oledOK) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("Box injoignable");
      display.println("");
      display.print("WiFi : "); display.println(AP_SSID);
     // display.println("http://192.168.5.1");
      display.print("http://"); display.println(WiFi.softAPIP());
      display.display();
    }
    delay(2000);
  } else {
    Serial.println("WiFi connected.");
    Serial.print("Reseau utilise : ");
    Serial.println(WiFi.SSID());
    Serial.print("IP address: ");
    // Affiche l'adresse IP locale attribuée à l'ESP32 (à utiliser dans le navigateur)
    Serial.println(WiFi.localIP());
  }
  if (AP_SECOURS_TOUJOURS_ACTIF) demarrerReseauSecours();

  // Démarre le service mDNS : l'ESP32 devient joignable via http://richardv.local
  // en plus de son adresse IP (pratique si l'IP change au fil du temps).
  if (MDNS.begin(hostname)) {
    Serial.print("mDNS actif : http://");
    Serial.print(hostname);
    Serial.println(".local");
    MDNS.addService("http", "tcp", 80); // Annonce le service web sur le port 80
  } else {
    Serial.println("Erreur lors du demarrage du mDNS");
  }

  // 📡 Configure et démarre la mise à jour du firmware par WiFi (voir setupOTA()
  // plus haut). Placée après la connexion WiFi et le mDNS, dont l'OTA a besoin.
  setupOTA();

  updateOLED(0); // Première mise à jour de l'écran avec le réseau/IP obtenus

  // Synchronise l'horloge interne de l'ESP32 via NTP (serveurs de temps en ligne),
  // en appliquant le fuseau horaire français défini plus haut (TZ_INFO)
  sntp_set_time_sync_notification_cb(ntpSynchronise); // 🕒 Prévenu à chaque synchro NTP réussie
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

 // 🚩3️⃣🚨 Attente active de la synchronisation de l'heure NTP
  Serial.print("Attente de la synchronisation NTP ");
  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Synchro heure ...");
    display.display();
  }

  struct tm timeTesting;
  int tentative = boxAuDemarrage ? 0 : 20; // 🛟 Sans box, inutile d'attendre le NTP
   // On met une limite à 20 tentatives (10 secondes) pour éviter de bloquer l'ESP32 si Internet est en panne
  while (!lireHeureLocale(&timeTesting) && tentative < 20) {
    delay(500);
    Serial.print(".");
    tentative++;
  }
  Serial.println("");

  if (tentative >= 20) {
    Serial.println("⏰ NTP Timeout : Demarrage sans heure valide (reglage manuel possible depuis la page web)");
  } else {
    char afficheHeure[30];
    strftime(afficheHeure, sizeof(afficheHeure), "%H:%M:%S", &timeTesting);
    Serial.printf("⏰ Heure synchronisee avec succes : %s\n", afficheHeure);
  }

  // Corrige IMMÉDIATEMENT l'état physique des relais en mode automatique
  // maintenant que l'heure est connue, au lieu d'attendre jusqu'à 1 s de plus
  // (le temps du premier passage dans loop()). Important après une mise à
  // jour OTA : sans cet appel, un relais qui était ON avant le flash reste
  // physiquement ON (état chargé depuis la NVS tout au début de setup(),
  // voir digitalWrite() juste après loadSettings()) jusqu'au prochain tick
  // de loop() - ce qui, en cas de souci WiFi/NTP passager, pouvait retarder
  // la correction plus que nécessaire.
  appliquerProgrammation();

  updateOLED(0); // Fin de 3️⃣ Première mise à jour de l'écran avec le réseau/IP obtenus et la bonne heure

  // --------------------------------------------------------------------
  //  DÉFINITION DES ROUTES HTTP DU SERVEUR WEB
  //  Ces routes sont désormais GÉNÉRIQUES : une seule route par action,
  //  paramétrée par "id" (ex: /set-mode avec id=3), au lieu d'une route
  //  dédiée par relais. Elles fonctionnent donc pour n'importe quel nombre
  //  de relais déclarés dans programmateurs[].
  // --------------------------------------------------------------------

  // Route "/" (GET) : sert la page HTML principale, directement depuis la
  // mémoire flash (PROGMEM), sans passer par un système de fichiers.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", index_html);
    // Empêche le navigateur de mettre cette page en cache : sans cet
    // en-tête, un changement du HTML/CSS/JS embarqué (index_html) peut ne
    // pas apparaître après une mise à jour OTA tant que le cache du
    // navigateur n'est pas vidé manuellement (Ctrl+F5), ce qui peut donner
    // l'impression à tort que la mise à jour n'a pas fonctionné.
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // Route "/get-config" (GET) : décrit à la page web la liste des relais à
  // afficher (id, nom, sous-titre, couleur). Appelée une fois au chargement
  // de la page, avant de construire les lignes de l'interface.
  server.on("/get-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    Verrou v; 
    JsonDocument doc;
    JsonArray arr = doc["relais"].to<JsonArray>();
    for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = programmateurs[i].id;
      o["name"] = programmateurs[i].nomAff;     //  nom modifiable depuis la page web
      o["sub"] = programmateurs[i].sousNomAff;  //  sous-titre modifiable
      o["nameDef"] = programmateurs[i].nom;     // Noms par défaut (bouton "Noms d'origine")
      o["subDef"] = programmateurs[i].sousNom;
      o["color"] = programmateurs[i].couleur;
    }
    // La page web ne connaît pas la limite choisie côté firmware : on la
    // lui transmet ici (nombre maximum de plages par relais), pour qu'elle
    // désactive le bouton "Ajouter une plage" au bon moment sans rien coder
    // en dur. Changez MAX_PLAGES en haut du fichier, et l'interface suit.
    doc["maxPlages"] = MAX_PLAGES;
    doc["maxNom"] = MAX_LONGUEUR_NOM;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Route "/get-data" (GET) : renvoie en JSON l'état complet de TOUS les
  // programmateurs (un objet imbriqué par id) + l'heure courante. Interrogée
  // chaque seconde par le JavaScript de la page (fonction update()).
  server.on("/get-data", HTTP_GET, [](AsyncWebServerRequest *request) {
    Verrou v; // lecture cohérente des relais pendant que loop() tourne
    JsonDocument doc;

    // Récupère et formate l'heure courante (HH:MM) pour l'affichage
    struct tm timeinfo;
    char buff[10];
    bool heureOK = lireHeureLocale(&timeinfo);
    if (heureOK) strftime(buff, sizeof(buff), "%H:%M", &timeinfo); // Heure valide : formatage
    else sprintf(buff, "--:--");                                   // Heure non synchronisée : placeholder
    int w = heureOK ? minuteSemaine(timeinfo) : -1; // minute de la semaine

    for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
      Programmateur &p = programmateurs[i];
      JsonObject o = doc[p.id].to<JsonObject>();
      o["plages"] = plagesVersTexteBrut(p.plages);       // Toutes les plages, triées : "06:30-08:00,11:30-13:15/12345"
      o["resume"] = plagesVersTexte(p.plages, 3, true);  // Texte lisible : "06:30-08:00 (L-V), 11:30-13:15 +1"
      o["auto"]   = p.modeAuto;
      o["etat"]   = p.relayState;
      // Forçage temporaire : secondes restantes (-1 = aucun)
      long reste = (p.modeAuto && p.forcageActif) ? forcageRestantSec(p) : -1;
      o["forcage"] = reste;
      // Minutes avant le prochain changement d'état programmé. -1 = aucun
      // changement prévu (aucune plage, plages couvrant toute la semaine,
      // mode manuel, forçage en cours, ou heure pas synchronisée).
      o["restant"] = (heureOK && p.modeAuto && reste < 0) ? plagesProchainChangement(p.plages, w) : -1;

      //  Plages (dans l'ordre de "plages") actives en ce moment, et la
      // prochaine à démarrer : la page les colore en vert / rouge. Calculé ici
      // car l'ESP32 seul sait appliquer les jours de la semaine.
      JsonArray act = o["actives"].to<JsonArray>();
      int suivante = -1;
      if (heureOK) {
        Plage tri[MAX_PLAGES];
        int n = trierPlages(p.plages, tri);
        int meilleur = MIN_SEMAINE + 1;
        for (int k = 0; k < n; k++) {
          if (plageActiveA(tri[k], w)) { act.add(k); continue; }
          int d = prochainDebutPlage(tri[k], w);
          if (d >= 0 && d < meilleur) { meilleur = d; suivante = k; }
        }
      }
      o["suivante"] = suivante;
    }

    doc["actuelle"] = String(buff);
    doc["jour"] = heureOK ? (w / 1440) : -1; //  0 = lundi ... 6 = dimanche
    doc["heureOK"] = heureOK;           // 🕒 false -> la page affiche un bandeau "Heure non réglée"
    doc["heureSource"] = sourceHeure(); // 🕒 "ntp", "manuelle" ou "aucune"
    doc["build"] = FIRMWARE_BUILD;      //  permet à la page de se recharger après une OTA

    // 📶 Qualité du signal WiFi en %, affichée à côté du bouton "Infos système"
    // (voir rssiToPercent() plus haut). -1 = non connecté, pour que le JS affiche "--".
    doc["wifiPct"] = boxConnectee() ? rssiToPercent(WiFi.RSSI()) : -1;

    String json;
    serializeJson(doc, json);                          // Convertit le document JSON en chaîne de caractères
    request->send(200, "application/json", json);      // Renvoie la réponse HTTP 200 avec le JSON
  });

  // Route "/get-info" (GET) : renvoie en JSON les informations système
  // (état WiFi, nom mDNS, IP, adresse MAC, puissance du signal). Utilisée
  // par le popup "?" affiché sur la page web.
  server.on("/get-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;

    bool connected = boxConnectee(); 
    doc["connected"] = connected;
    doc["ssid"] = connected ? WiFi.SSID() : "--"; // Nom du réseau WiFi actuellement connecté
    doc["hostname"] = String(hostname) + ".local";
    doc["ip"] = connected ? WiFi.localIP().toString() : "--";
    doc["mac"] = WiFi.macAddress(); // Adresse MAC de la carte ESP32 (toujours disponible)
    doc["rssi"] = connected ? (String(WiFi.RSSI()) + " dBm") : "--";
    doc["rssiPct"] = connected ? rssiToPercent(WiFi.RSSI()) : -1; // Même conversion que /get-data, pour le popup
    doc["build"] = FIRMWARE_BUILD; // Signature de build : preuve qu'une OTA a bien pris effet
    // Permet de vérifier, sans moniteur série, si la structure NVS a dû être
    // réinitialisée. Une simple mise à jour OTA ne remet donc plus les horaires à zéro.
    doc["nvsReset"] = nvsReinitialiseeAuDemarrage;
    doc["buildPrecedent"] = buildPrecedentNVS.length() ? buildPrecedentNVS : "(1er demarrage)";
    // 🛟 État du réseau de secours
    doc["apActif"] = apSecoursActif;
    doc["apSsid"] = AP_SSID;
    doc["apIp"] = apSecoursActif ? WiFi.softAPIP().toString() : String("--");
    doc["apClients"] = apSecoursActif ? WiFi.softAPgetStationNum() : 0;
    doc["heureSource"] = sourceHeure();
    //  diagnostic des redémarrages et de la mémoire
    doc["reset"] = raisonRedemarrage();
    doc["uptime"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();
    doc["heapMin"] = ESP.getMinFreeHeap();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // --------------------------------------------------------------------
  //  ROUTES DE COMMANDE EXPLICITES (POST)
  //  Elles indiquent l'état VOULU (et non "inverse l'état") : envoyer deux
  //  fois la même commande donne le même résultat. Un double appui, une page
  //  restée ouverte avec des données périmées ou une requête rejouée par le
  //  navigateur ne peuvent donc plus inverser un relais par erreur.
  //  Les anciennes routes /toggle-mode et /force-state (bascules en GET)
  //  sont supprimées. Exemples d'appel depuis un script :
  //    curl -X POST http://richardv.local/set-state -d "id=2&etat=1"
  //    curl -X POST http://richardv.local/set-state -d "id=1&etat=1&duree=30"
  //    curl -X POST http://richardv.local/all -d "action=auto"
  // --------------------------------------------------------------------

  // "/set-mode" : id, auto (1 = AUTO, 0 = MANUEL). Passer en AUTO annule aussi
  // un éventuel forçage temporaire (c'est le bouton "Annuler" de la page).
  server.on("/set-mode", HTTP_POST, [](AsyncWebServerRequest *request) {
    String id, a;
    if (!lireParam(request, "id", id) || !lireParam(request, "auto", a)) {
      request->send(400, "text/plain", "Parametres 'id' et 'auto' requis"); return;
    }
    Verrou v;
    Programmateur* p = findProg(id);
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }
    p->modeAuto = (a == "1");
    p->forcageActif = false;
    saveRelaySettings(*p);                            // 💾 Seul ce relais est réécrit en NVS
    if (p->modeAuto) appliquerProgrammation();        // Applique immédiatement l'horaire en AUTO
    else ecrireRelais(*p, p->relayState);    // Réapplique l'état mémorisé en MANUEL
    request->send(200, "text/plain", "OK");
  });

  // "/set-state" : id, etat (1 = ON, 0 = OFF), duree (optionnelle).
  //  - sans "duree"        : forçage PERMANENT (passage en mode MANUEL) ;
  //  - duree=15 (minutes)  : forçage TEMPORAIRE, puis retour automatique en AUTO
  //                          (1 à DUREE_FORCAGE_MAX minutes) ;
  //  - duree=prochain      : forçage temporaire jusqu'au prochain changement
  //                          programmé (nécessite l'heure et au moins une plage).
  server.on("/set-state", HTTP_POST, [](AsyncWebServerRequest *request) {
    String id, e, duree;
    if (!lireParam(request, "id", id) || !lireParam(request, "etat", e)) {
      request->send(400, "text/plain", "Parametres 'id' et 'etat' requis"); return;
    }
    Verrou v;
    Programmateur* p = findProg(id);
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }
    bool etat = (e == "1");

    if (!lireParam(request, "duree", duree)) {
      // Forçage permanent (mode MANUEL), comme le bouton "Forcer" de la V3
      p->modeAuto = false;
      p->forcageActif = false;
      p->relayState = etat;
      ecrireRelais(*p, etat);
      saveRelaySettings(*p);
      request->send(200, "text/plain", "OK");
      return;
    }

    long minutes;
    if (duree == "prochain") {
      struct tm t;
      int d = lireHeureLocale(&t) ? plagesProchainChangement(p->plages, minuteSemaine(t)) : -1;
      if (d < 0) {
        request->send(400, "text/plain", "Aucun changement programme a venir (ou heure non reglee) : choisissez une duree");
        return;
      }
      minutes = d;
    } else {
      minutes = duree.toInt();
      if (minutes < 1 || minutes > DUREE_FORCAGE_MAX) {
        request->send(400, "text/plain", "Duree invalide (1 a " + String(DUREE_FORCAGE_MAX) + " min)");
        return;
      }
    }
    bool etaitManuel = !p->modeAuto;
    p->modeAuto = true; // Le forçage temporaire se fait "par-dessus" le mode AUTO
    demarrerForcage(*p, etat, minutes);
    if (etaitManuel) saveRelaySettings(*p); // Mémorise le retour en AUTO (pas le forçage lui-même)
    appliquerProgrammation();
    Serial.printf("Forcage temporaire de %s : %s pendant %ld min\n", p->id, etat ? "ON" : "OFF", minutes);
    request->send(200, "text/plain", "OK");
  });

  // "/all" : action = on | off | auto. Commande groupée traitée d'un seul coup
  // par l'ESP32 (avant : une série de bascules envoyées une par une par la page).
  server.on("/all", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if (!lireParam(request, "action", action) ||
        (action != "on" && action != "off" && action != "auto")) {
      request->send(400, "text/plain", "Parametre 'action' = on, off ou auto"); return;
    }
    Verrou v;
    for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
      Programmateur &p = programmateurs[i];
      p.forcageActif = false;
      if (action == "auto") {
        p.modeAuto = true;
      } else {
        p.modeAuto = false;
        p.relayState = (action == "on");
        ecrireRelais(p, p.relayState);
      }
    }
    saveSettings();            // 💾 Une seule sauvegarde pour tous les relais
    appliquerProgrammation();
    request->send(200, "text/plain", "OK");
  });

  // Route "/save?id=..." (POST) : reçoit la nouvelle programmation du
  // programmateur désigné par "id", sous la forme d'un paramètre "plages" :
  // un texte "06:30-08:00,18:45-22:30/67" (une seule requête suffit, quel que
  // soit le nombre de plages définies).  "/12345" après une plage =
  // jours où elle s'applique (1 = lundi ... 7 = dimanche ; "1-5" accepté).
  // C'est le format envoyé par la page web (voir saveGrid()), mais aussi
  // pratique pour piloter l'ESP32 depuis un script, sans passer par la page :
  //   curl -X POST "http://richardv.local/save?id=1" -d "plages=06:30-08:00/1-5,18:45-22:30"
  // Une plage vide ("plages=") efface toutes les plages du relais.
  // Un texte invalide (ou plus de MAX_PLAGES plages) est refusé en bloc.
  // (Accès sans authentification, comme toutes les routes de la page.)
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    String id, texteRecu;
    if (!lireParam(request, "id", id)) { request->send(400, "text/plain", "id manquant"); return; }
    if (!lireParam(request, "plages", texteRecu)) {
      // Si le paramètre manque, on répond explicitement plutôt que de ne rien envoyer
      // (évite l'erreur "Handler did not handle the request")
      request->send(400, "text/plain", "Parametre 'plages' manquant");
      return;
    }
    Verrou v;
    Programmateur* p = findProg(id);
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }
    // Refuse une programmation invalide sans détruire celle déjà enregistrée.
    if (!textePlagesValide(texteRecu)) {
      request->send(400, "text/plain", "Format de plages invalide");
      return;
    }
    texteVersPlages(texteRecu.c_str(), p->plages);
    saveRelaySettings(*p);     // 💾 Sauvegarde ciblée : seul ce relais est réécrit en NVS
    appliquerProgrammation();  // Applique tout de suite les nouvelles plages, sans attendre le prochain tick
    request->send(200, "text/plain", "OK : " + plagesVersTexte(p->plages, 0, true));
  });

  // 🔆 "/get-oled" (GET) : réglages actuels de luminosité de l'écran OLED.
  server.on("/get-oled", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    {
      Verrou v;
      doc["jour"] = oledLumJour;
      doc["nuit"] = oledLumNuit;
      doc["nuitActive"] = oledNuitActive;
      doc["debut"] = minutesEnHm(oledNuitDebut);
      doc["fin"] = minutesEnHm(oledNuitFin);
      doc["enNuit"] = estModeNuit();
      doc["ecran"] = oledOK;
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // 🔆 "/set-oled" (POST) : jour (1-100), nuit (0-100, 0 = éteint),
  // nuitActive (0/1), debut et fin ("HH:MM"). Tous les champs sont requis.
  //   curl -X POST http://richardv.local/set-oled -d "jour=80&nuit=0&nuitActive=1&debut=22:30&fin=06:45"
  server.on("/set-oled", HTTP_POST, [](AsyncWebServerRequest *request) {
    String j, n, a, d, f;
    if (!lireParam(request, "jour", j) || !lireParam(request, "nuit", n) ||
        !lireParam(request, "nuitActive", a) || !lireParam(request, "debut", d) ||
        !lireParam(request, "fin", f)) {
      request->send(400, "text/plain", "Parametres jour, nuit, nuitActive, debut, fin requis"); return;
    }
    int lj = j.toInt(), ln = n.toInt();
    int md = hmEnMinutes(d), mf = hmEnMinutes(f);
    if (lj < 1 || lj > 100 || ln < 0 || ln > 100 || md < 0 || mf < 0 || md >= 1440 || mf >= 1440) {
      request->send(400, "text/plain", "Valeurs invalides"); return;
    }
    if (a == "1" && md == mf) {
      request->send(400, "text/plain", "Debut et fin du mode nuit identiques"); return;
    }
    {
      Verrou v;
      oledLumJour = lj;
      oledLumNuit = ln;
      oledNuitActive = (a == "1");
      oledNuitDebut = md;
      oledNuitFin = mf;
      oledApercuNiveau = -1; // Fin d'un éventuel aperçu : loop() applique le bon niveau
    }
    sauvegarderOled();
    request->send(200, "text/plain", "OK");
  });

  // 🔆 "/oled-apercu" (POST) : niveau (0-100). Applique ce niveau pendant
  // quelques secondes (OLED_APERCU_MS) sans rien enregistrer : sert à voir le
  // rendu des curseurs de la page web pendant le réglage.
  server.on("/oled-apercu", HTTP_POST, [](AsyncWebServerRequest *request) {
    String n;
    if (!lireParam(request, "niveau", n)) { request->send(400, "text/plain", "niveau manquant"); return; }
    int niv = n.toInt();
    if (niv < 0 || niv > 100) { request->send(400, "text/plain", "niveau 0 a 100"); return; }
    {
      Verrou v;
      oledApercuNiveau = niv;
      oledApercuDebut = millis();
    }
    request->send(200, "text/plain", "OK");
  });

  //  "/set-noms" (POST) : id, nom, sous. Renomme une programmation
  // (affichage de la page web). Un champ vide = nom d'origine.
  server.on("/set-noms", HTTP_POST, [](AsyncWebServerRequest *request) {
    String id, nom, sous;
    if (!lireParam(request, "id", id)) { request->send(400, "text/plain", "id manquant"); return; }
    lireParam(request, "nom", nom);
    lireParam(request, "sous", sous);
    nom = nettoyerNom(nom);
    sous = nettoyerNom(sous);
    Verrou v;
    Programmateur* p = findProg(id);
    if (!p) { request->send(404, "text/plain", "id inconnu"); return; }
    p->nomAff = nom.length() ? nom : String(p->nom);
    p->sousNomAff = sous.length() ? sous : String(p->sousNom);
    saveNoms(*p);
    request->send(200, "text/plain", "OK");
  });

  // Route "/reset-auto" (GET) : repasse TOUS les relais en mode
  // automatique en une seule fois (et annule les forçages temporaires).
  // Utile après une mise à jour du firmware (OTA) si un ou plusieurs relais
  // étaient restés forcés en mode MANUEL avant le flash. Ne touche pas aux
  // horaires enregistrés. Gardée en GET pour rester utilisable en tapant
  // simplement http://richardv.local/reset-auto dans un navigateur (elle
  // donne toujours le même résultat, même appelée plusieurs fois).
  server.on("/reset-auto", HTTP_GET, [](AsyncWebServerRequest *request) {
    Verrou v;
    for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
      programmateurs[i].modeAuto = true;
      programmateurs[i].forcageActif = false;
    }
    saveSettings();            // 💾 Sauvegarde globale (mode de tous les relais)
    appliquerProgrammation();  // Applique tout de suite les horaires en cours
    request->send(200, "text/plain", "OK : tous les relais sont repasses en mode Automatique");
  });

  // 🕒 Route "/set-time" (POST) : réglage manuel de l'horloge de l'ESP32.
  // Deux formats acceptés :
  //   - "epoch"    : secondes depuis le 01/01/1970 (UTC), envoyé par le bouton
  //                  "Prendre l'heure du smartphone" (indépendant du fuseau du téléphone)
  //   - "datetime" : "AAAA-MM-JJTHH:MM" (heure LOCALE française), saisie manuelle
  // Si le NTP redevient joignable plus tard, il recorrige l'heure automatiquement.
  server.on("/set-time", HTTP_POST, [](AsyncWebServerRequest *request) {
    time_t t = 0;
    if (request->hasParam("epoch", true)) {
      t = (time_t) atoll(request->getParam("epoch", true)->value().c_str());
    } else if (request->hasParam("datetime", true)) {
      int Y, M, D, h, m;
      String v = request->getParam("datetime", true)->value();
      if (sscanf(v.c_str(), "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) != 5 ||
          M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59) {
        request->send(400, "text/plain", "Format de date invalide");
        return;
      }
      struct tm tmv = {};
      tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
      tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = 0;
      tmv.tm_isdst = -1;   // Laisse le fuseau TZ_INFO déterminer heure d'été / d'hiver
      t = mktime(&tmv);    // Heure locale -> secondes UTC
    } else {
      request->send(400, "text/plain", "Parametre 'epoch' ou 'datetime' manquant");
      return;
    }
    if (t < 1704067200) { // Avant le 01/01/2024 : forcément une erreur de saisie
      request->send(400, "text/plain", "Date invalide");
      return;
    }
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    heureManuelle = true;
    appliquerProgrammation(); // Les relais en mode AUTO suivent immédiatement la nouvelle heure
    struct tm verif;
    char txt[24] = "--";
    if (lireHeureLocale(&verif)) strftime(txt, sizeof(txt), "%d/%m/%Y %H:%M", &verif);
    Serial.printf("Heure reglee manuellement : %s\n", txt);
    request->send(200, "text/plain", String("OK : ") + txt);
  });

  // Route "attrape-tout" : répond proprement (404) à toute URL non reconnue
  // par les routes ci-dessus, plutôt que de laisser une réponse vide.
  //  PORTAIL CAPTIF : sur le réseau de secours, toute adresse inconnue
  // est redirigée vers la page du programmateur. C'est ce qui fait apparaître
  // "Se connecter au réseau" sur le smartphone : il teste sa connexion avec
  // des adresses comme /generate_204 (Android), /hotspot-detect.html (iPhone)
  // ou /connecttest.txt (Windows) et, recevant une redirection, ouvre
  // automatiquement la page. Sur le réseau de la box, réponse 404 normale.
  server.onNotFound([](AsyncWebServerRequest *request) {
    bool viaSecours = apSecoursActif && request->client() &&
                      request->client()->localIP() == WiFi.softAPIP();
    if (viaSecours && AP_INTERNET_SIMULE) {
      //  réponses "Internet OK" attendues par chaque système
      String u = request->url();
      if (u == "/generate_204" || u == "/gen_204") {                 // Android / Chrome
        request->send(204); return;
      }
      if (u == "/hotspot-detect.html" || u == "/library/test/success.html") { // iPhone / Mac
        request->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        return;
      }
      if (u == "/connecttest.txt") { request->send(200, "text/plain", "Microsoft Connect Test"); return; } // Windows
      if (u == "/ncsi.txt")        { request->send(200, "text/plain", "Microsoft NCSI"); return; }         // Windows (ancien)
      if (u == "/success.txt")     { request->send(200, "text/plain", "success\n"); return; }             // Firefox
    }
    if (viaSecours) {
      String url = "http://" + WiFi.softAPIP().toString() + "/";
      request->redirect(url.c_str()); // c_str() : compatible avec toutes les versions d'ESPAsyncWebServer
      return;
    }
    request->send(404, "text/plain", "Not found");
  });

  server.begin(); // Démarre effectivement le serveur web (les routes deviennent actives)
}

// ============================================================================
//  APPLICATION DE LA PROGRAMMATION HORAIRE (mode automatique)
// ============================================================================
//  Recalcule l'état voulu de chaque relais et l'applique sur la broche
//  physique dès qu'il diffère de l'état courant. Fonction extraite de loop()
//  pour pouvoir être appelée à DEUX endroits :
//   1) une fois dans setup(), juste après la synchronisation NTP, afin de
//      corriger tout de suite un relais resté sur son ANCIEN état (celui
//      chargé depuis la NVS au tout début de setup(), potentiellement
//      obsolète après une mise à jour OTA), au lieu d'attendre jusqu'à 1 s
//      de plus (le temps du premier passage dans loop()) ;
//   2) à chaque seconde dans loop(), comme avant, pour suivre les horaires
//      en continu.
//  ⚠️ Les relais en mode MANUEL ne sont PAS concernés par le recalcul
//  horaire : leur état est entièrement piloté par l'utilisateur (page web
//  "FORCER ON/OFF", ou bouton poussoir physique - voir checkPhysicalButtons()).
//  Un relais resté en mode manuel après une mise à jour continuera donc à
//  ignorer les nouveaux horaires tant qu'il n'aura pas été rebasculé en Auto
//  (via le bouton de la page web, ou la nouvelle route /reset-auto ci-dessous).
void appliquerProgrammation() {
  Verrou v; //  appelée aussi bien par loop() que par les routes web

  // Récupère l'heure courante et la formate en "HH:MM"
  struct tm timeinfo;
  bool heureValide = lireHeureLocale(&timeinfo); // false si le NTP n'est pas (encore) synchronisé
  int w = -1; // Minute de la SEMAINE (0 = lundi 00:00), pour les plages par jour
  if (heureValide) {
    char nowStr[6];
    strftime(nowStr, sizeof(nowStr), "%H:%M", &timeinfo);
    now = String(nowStr);
    w = minuteSemaine(timeinfo);
  }

  // --- Logique de programmation : identique pour tous les relais déclarés,
  //     appliquée en boucle sur le tableau programmateurs[] ---
  for (int i = 0; i < NB_PROGRAMMATEURS; i++) {
    Programmateur &p = programmateurs[i];

    // FORÇAGE TEMPORAIRE : prioritaire sur la programmation tant qu'il dure
    if (p.modeAuto && p.forcageActif) {
      if (forcageRestantSec(p) > 0) {
        p.relayState = p.forcageEtat;
        ecrireRelais(p, p.relayState);
        continue;
      }
      p.forcageActif = false; // Forçage terminé : la programmation reprend ci-dessous
      Serial.printf("Fin du forcage temporaire de %s : retour en AUTO\n", p.id);
    }

    //***🚨⌚️ Programmation horaire
    if (p.modeAuto && heureValide) { // 🔧 On n'applique la logique horaire QUE si l'heure est fiable
      // L'état voulu se lit DIRECTEMENT dans la liste de plages : on regarde si
      // la minute courante (de la semaine) tombe dans l'une d'elles.
      // Le passage par minuit (ex: 22:00 -> 06:00) et les jours de la semaine
      // sont gérés par plageActiveA().
      bool newState = plageEstActive(p.plages, w);
      // on réécrit TOUJOURS la broche physique, même si l'état
      // calculé est identique à p.relayState : après un redémarrage, la valeur
      // rechargée depuis la NVS peut être obsolète (les transitions automatiques
      // n'y sont pas sauvegardées), et la sortie réelle doit être corrigée.
      p.relayState = newState;
      ecrireRelais(p, p.relayState);
    } else {
      // Mode manuel, OU mode auto mais heure pas encore synchronisée :
      // on se contente de réappliquer l'état mémorisé, sans le recalculer.
      ecrireRelais(p, p.relayState);
    }
  }
}

// ============================================================================
//  BOUCLE PRINCIPALE (exécutée en continu après setup())
// ============================================================================
void loop() {

  // static : ces variables conservent leur valeur d'un passage à l'autre de loop()
  static unsigned long lastCheck = 0;
  static unsigned long lastPageChange = millis();
  static int pageOLED = 0;

  // 🔘 Vérifie les boutons poussoirs de forçage physique à CHAQUE passage de
  // loop() (et non une fois par seconde comme le reste) pour une réactivité
  // immédiate à l'appui, avec anti-rebond géré en interne.
  checkPhysicalButtons();

  // 📡 Traite les requêtes de mise à jour OTA en attente. Comme checkPhysicalButtons(),
  // appelée à CHAQUE passage de loop() (et non une fois par seconde) pour que
  // l'ESP32 réponde sans délai à une demande de flash depuis l'IDE Arduino.
  ArduinoOTA.handle(); // 🔄 Mode OTA

  //  répond aux requêtes DNS du réseau de secours (portail captif)
  if (apSecoursActif) dnsServer.processNextRequest();

  // N'exécute le bloc ci-dessous qu'une fois par seconde (1000 ms), pour ne
  // pas surcharger inutilement le processeur (millis() ne bloque jamais,
  // contrairement à delay())
  if (millis() - lastCheck >= 1000) {
    lastCheck = millis(); // Mémorise l'instant de ce passage pour la prochaine comparaison

    // Surveillance de la connexion WiFi : si elle a été coupée (box redémarrée,
    // hors de portée...), on relance une recherche + connexion au meilleur
    // réseau connu disponible. Version NON BLOQUANTE (voir startBestNetworkConnect/
    // pollBestNetworkConnect plus haut) : on ne démarre une nouvelle tentative
    // que toutes les 10 s, et on fait avancer d'un cran une tentative déjà en
    // cours à chaque passage ici, sans jamais figer loop() en l'attendant.
    static unsigned long lastWifiRetry = millis();
    // 🛟 Intervalle entre deux recherches de la box : allongé quand le réseau
    // de secours est ouvert, pour ne pas déranger le smartphone connecté.
    unsigned long intervalleRetry = WIFI_RETRY_NORMAL;
    if (apSecoursActif) {
      intervalleRetry = (WiFi.softAPgetStationNum() > 0) ? WIFI_RETRY_AP_AVEC_CLIENT
                                                          : WIFI_RETRY_AP_SANS_CLIENT;
    }
    // une tentative en cours est TOUJOURS menée
    // à son terme, même si le WiFi est déjà connecté. Avant, la tentative
    // n'avançait que si "WiFi.status() != WL_CONNECTED" : dès que la box
    // acceptait la connexion, ce test devenait faux, pollBestNetworkConnect()
    // n'était plus jamais appelée et la machine à états restait BLOQUÉE sur
    // "connexion en cours". Au 1er cycle tout allait bien (la connexion du
    // démarrage passe par setup()), mais à la coupure suivante cet état
    // périmé déclarait la box "en échec" (liste noire) et décalait toutes
    // les étapes : d'où des cycles 2, 3... de plus en plus lents.
    if (wifiConnStep != WCS_IDLE) {
      {
        WifiConnResult r = pollBestNetworkConnect();
        if (r == WCR_CONNECTED) {
          Serial.print("Reconnecte a : ");
          Serial.println(WiFi.SSID());
          if (apSecoursActif) {   
            Serial.printf("Box retrouvee alors que le secours est ouvert (%d smartphone(s)) : "
                          "fermeture du secours dans %lu s\n",
                          WiFi.softAPgetStationNum(), AP_DELAI_DESACTIVATION / 1000);
          }
        }
        // WCR_PENDING : rien à faire, on continuera au prochain passage
        // WCR_FAILED  : rien à faire non plus, un nouvel essai sera tenté plus tard
      }
    } else if (!boxConnectee()) { 
      // quand l'ouverture du secours est due (box perdue depuis
      // AP_DELAI_ACTIVATION), on ne lance PAS de nouveau scan : un scan en
      // cours empêche d'ouvrir le point d'accès (voir demarrerReseauSecours()),
      // et l'enchaînement des scans retardait le secours de façon aléatoire.
      // Fenêtre limitée à 15 s : si le secours ne peut pas s'ouvrir (mot de
      // passe trop court...), la recherche de la box reprend normalement.
      // boxPerdueDepuis n'est renseignée qu'APRÈS ce bloc (dans
      // gererReseauSecours()) : le tout 1er scan suivant la perte de la box est
      // donc bien lancé, puis plus aucun jusqu'à l'ouverture du secours. Avant,
      // la condition n'était vraie qu'à partir de AP_DELAI_ACTIVATION, et comme
      // WIFI_RETRY_NORMAL a la même valeur, un 2e scan partait juste avant.
      bool secoursEnAttente = !apSecoursActif && !AP_SECOURS_TOUJOURS_ACTIF &&
                              boxPerdueDepuis != 0 &&
                              millis() - boxPerdueDepuis < AP_DELAI_ACTIVATION + 15000;
      if (!secoursEnAttente && millis() - lastWifiRetry >= intervalleRetry) {
        lastWifiRetry = millis();
        Serial.println("WiFi deconnecte, nouvelle recherche de reseau...");
        startBestNetworkConnect();
      }
    }

    // Même si on est déjà connecté, on vérifie de temps en temps si le
    // réseau habituellement le meilleur est redevenu disponible, pour ne pas
    // rester bloqué indéfiniment sur un réseau de repli. checkForBetterNetwork()
    // gère maintenant elle-même son minuteur de 60 s et son scan asynchrone
    // (voir plus haut) : on l'appelle donc simplement à chaque tick.
    checkForBetterNetwork(); // 🚩4️⃣  Commutation Réseau

    gererReseauSecours(); // 🛟 Ouvre / ferme le réseau "ESP32_Secours" selon l'état de la box

    // Recalcule et applique l'état de tous les relais en mode automatique
    // (fonction extraite plus haut, voir appliquerProgrammation() pour le détail
    // - aussi appelée une fois dans setup() juste après la synchro NTP).
    appliquerProgrammation();

    // Avance la page de l'écran OLED toutes les 8 secondes (utile seulement
    // si le nombre de relais dépasse OLED_LIGNES_PAR_PAGE, sinon updateOLED
    // affichera toujours la même page unique). avant
    if (millis() - lastPageChange >= 8000) { //⏰ Tempo pour basculement page suivante
      lastPageChange = millis();
      pageOLED++;
    }
    {
      Verrou v; //  l'écran lit les relais/plages, que la page web peut modifier au même moment
      gererLuminositeOLED(); // 🔆 Luminosité jour / nuit / aperçu (commande envoyée seulement si elle change)
      updateOLED(pageOLED); // Rafraîchit l'écran OLED avec le réseau/IP actuels et l'état des relais
    }
  }
}
