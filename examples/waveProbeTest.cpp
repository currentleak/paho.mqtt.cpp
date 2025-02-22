
// sudo g++ -o Wave1 waveProbeTest.cpp -lfftw3 -lpaho-mqttpp3 -lpaho-mqtt3as

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <cmath>                   // pour sqrt()
#include "mqtt/async_client.h"      // Bibliothèque MQTT (flag de linker : -lpaho-mqttpp3 -lpaho-mqtt3as)
#include "/home/kco/json/single_include/nlohmann/json.hpp"    // Bibliothèque nlohmann JSON (bibliothèque "header-only")
#include <fftw3.h>                                            // Bibliothèque FFTW (flag de linker : -lfftw3)

using json = nlohmann::json;
using namespace std;

const string DFLT_SERVER_URI{"mqtt://192.168.1.73:1883"};
const string CLIENT_ID{"PahoCppAsyncConsumeV5"};
const string TOPIC{"inspection/ascan"};
const int QOS = 1;
const int DEFAULT_AVERAGE_COUNT = 1;

int main(int argc, char* argv[])
{
    cout << "Wave - Probe Characterisation" << endl;
    
    // Valeurs par défaut
    string serverURI = DFLT_SERVER_URI;
    int averageCount = DEFAULT_AVERAGE_COUNT;
    bool recordCSV = false;  // Enregistrement CSV activé uniquement si -r est spécifié

    // Traitement des arguments avec options -a, -m et -r
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-a" && i + 1 < argc) {
            serverURI = argv[++i];
        }
        else if (arg == "-m" && i + 1 < argc) {
            try {
                averageCount = stoi(argv[++i]);
                if (averageCount < 1) {
                    cerr << "Le nombre de messages pour la moyenne doit être >= 1. Utilisation de la valeur par défaut (" 
                         << DEFAULT_AVERAGE_COUNT << ")." << endl;
                    averageCount = DEFAULT_AVERAGE_COUNT;
                }
            }
            catch (...) {
                cerr << "Argument invalide pour le nombre de messages. Utilisation de la valeur par défaut (" 
                     << DEFAULT_AVERAGE_COUNT << ")." << endl;
                averageCount = DEFAULT_AVERAGE_COUNT;
            }
        }
        else if (arg == "-r") {
            recordCSV = true;
        }
        else {
            cerr << "Argument inconnu : " << arg << endl;
        }
    }
    
    // Affichage des paramètres pour vérification
    cout << "Adresse du serveur MQTT : " << serverURI << endl;
    cout << "Nombre de messages pour la moyenne : " << averageCount << endl;
    cout << "Enregistrement CSV : " << (recordCSV ? "activé" : "désactivé") << endl;
    
    // Initialisation des fichiers CSV (seulement si activé)
    std::unique_ptr<std::ofstream> csvFile;       // Pour les mesures
    std::unique_ptr<std::ofstream> ascanCsvFile;    // Pour les données ascan moyennées
    bool headerWritten = false;
    string filename, ascanFilename;
    
    // Variables pour accumuler les moyennes sur 'averageCount' messages
    vector<double> sumValues(4, 0.0);   // Pour les 4 mesures
    int messageCounter = 0;
    
    // Pour le champ "ascan"
    vector<double> sumAscan;            // Sera redimensionné lors du premier message ascan
    bool ascanInitialized = false;
    
    mqtt::async_client cli(serverURI, CLIENT_ID);

    auto connOpts = mqtt::connect_options_builder::v5()
                        .clean_start(false)
                        .properties({{mqtt::property::SESSION_EXPIRY_INTERVAL, 604800}})
                        .finalize();

    // Gestionnaires de connexion/déconnexion
    try {
        cli.set_connection_lost_handler([](const std::string&) {
            cout << "*** Connection Lost ***" << endl;
        });
        cli.set_disconnected_handler([](const mqtt::properties&, mqtt::ReasonCode reason) {
            cout << "*** Disconnected. Reason [0x" << hex << int{reason} << "]: " << reason << " ***" << endl;
        });

        // Démarrer la consommation dès maintenant pour ne pas manquer de messages
        cli.start_consuming();

        // Connexion au serveur MQTT
        cout << "Connecting to the MQTT server..." << flush;
        auto tok = cli.connect(connOpts);
        auto rsp = tok->get_connect_response();

        if (rsp.get_mqtt_version() < MQTTVERSION_5) {
            cout << "\n  Did not get an MQTT v5 connection." << flush;
            exit(1);
        }

        if (!rsp.is_session_present()) {
            cout << "\n  Session not present on broker. Subscribing..." << flush;
            cli.subscribe(TOPIC, QOS)->wait();
        }
        cout << "\n  OK" << endl;
        
        // Envoi des paramètres de configuration avant la collecte
        try {
            vector<pair<string, string>> configMessages = {
                {"inspection/configuration/probe/frequency", R"({"value": 5})"},
                {"inspection/configuration/us/pulsetype", R"({"value": "spike"})"},
                {"inspection/configuration/us/rxmode", R"({"value": "pe})"},
                {"inspection/configuration/us/voltage", R"({"value": 200})"},
                {"inspection/configuration/us/filter", R"({"value": "Broadband low"})"},
                {"inspection/configuration/us/rectification", R"({"value": "full"})"},
                {"inspection/configuration/measurementselection/1", R"({"value": "G1_peak_amplitude"})"},
                {"inspection/configuration/measurementselection/2", R"({"value": "G1_peak_soundPath"})"},
                {"inspection/configuration/measurementselection/3", R"({"value": "G1_peak_surfaceDistance"})"},
                {"inspection/configuration/measurementselection/4", R"({"value": "G1_peak_depth"})"}
            };

            for (const auto& conf : configMessages) {
                auto pubmsg = mqtt::make_message(conf.first, conf.second);
                pubmsg->set_qos(QOS);
                cli.publish(pubmsg)->wait();  // Publication synchrone
                cout << "Configuration envoyée: " << conf.first << " => " << conf.second << endl;
            }
        }
        catch (const mqtt::exception& exc) {
            cerr << "Erreur lors de l'envoi des paramètres de configuration: " << exc.what() << endl;
            return 1;
        }
        
        cout << "\nWaiting for messages on topic: '" << TOPIC << "'" << endl;
        
        // Pour stocker les noms des mesures (pour l'en-tête du CSV)
        vector<string> headerNames;
        
        // Boucle de réception et de traitement des messages MQTT
        while (true) {
            auto msg = cli.consume_message();
            if (!msg)
                break;

            string payload = msg->to_string();
            
            try {
                // Parser le JSON reçu
                json j = json::parse(payload);
                
                // Traitement des mesures (on suppose 4 mesures : measurement.1 à measurement.4)
                for (int i = 1; i <= 4; i++) {
                    string key = "measurement." + to_string(i);
                    if(j.contains(key)) {
                        json meas = j[key];
                        // Récupération du nom de la mesure lors de la première accumulation
                        if(headerNames.size() < 4) {
                            string name = meas.value("name", "col" + to_string(i));
                            headerNames.push_back(name);
                        }
                        double value = meas.value("value", 0.0);
                        sumValues[i-1] += value;
                    }
                }
                
                // Traitement du champ "ascan" : tableau d'entiers (samples ADC à 125MHz)
                if(j.contains("ascan")) {
                    auto ascanData = j["ascan"];
                    if (ascanData.is_array()) {
                        // Initialisation du vecteur d'accumulation pour ascan lors de la première réception
                        if (!ascanInitialized) {
                            sumAscan.resize(ascanData.size(), 0.0);
                            ascanInitialized = true;
                        }
                        // Vérifier que la taille du tableau correspond à celle attendue
                        if (ascanData.size() == sumAscan.size()) {
                            for (size_t i = 0; i < ascanData.size(); i++) {
                                sumAscan[i] += ascanData[i].get<int>();  // Conversion en entier, accumulation en double
                            }
                        }
                        else {
                            cerr << "Taille des données ascan différente de celle attendue." << endl;
                        }
                    }
                }
                
                messageCounter++;
                
                // Dès que le nombre de messages atteint 'averageCount', calculer les moyennes
                if(messageCounter == averageCount) {
                    // Moyenne des mesures
                    vector<double> avgValues(4, 0.0);
                    for (int i = 0; i < 4; i++) {
                        avgValues[i] = sumValues[i] / messageCounter;
                    }
                    // Moyenne des données ascan (si initialisé)
                    vector<double> avgAscan;
                    if(ascanInitialized) {
                        avgAscan.resize(sumAscan.size(), 0.0);
                        for (size_t i = 0; i < sumAscan.size(); i++) {
                            avgAscan[i] = sumAscan[i] / messageCounter;
                        }
                    }
                    
                    // Écriture dans les fichiers CSV si l'enregistrement est activé (-r)
                    if(recordCSV) {
                        // Pour les mesures
                        if (!csvFile) {
                            auto t = std::time(nullptr);
                            std::tm tm = *std::localtime(&t);
                            std::ostringstream oss;
                            oss << "dataMeas_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
                            filename = oss.str();
                            csvFile = make_unique<ofstream>(filename, std::ios::out | std::ios::app);
                            if (!csvFile->is_open()) {
                                cerr << "Impossible d'ouvrir le fichier " << filename << endl;
                                return 1;
                            }
                        }
                        if (!headerWritten && !headerNames.empty()) {
                            for (size_t i = 0; i < headerNames.size(); i++) {
                                *csvFile << headerNames[i] << (i < headerNames.size()-1 ? "," : "\n");
                            }
                            headerWritten = true;
                        }
                        for (size_t i = 0; i < avgValues.size(); i++) {
                            *csvFile << avgValues[i] << (i < avgValues.size()-1 ? "," : "\n");
                        }
                        csvFile->flush();
                        
                        // Pour les données ascan moyennées
                        if(ascanInitialized) {
                            if (!ascanCsvFile) {
                                auto t = std::time(nullptr);
                                std::tm tm = *std::localtime(&t);
                                std::ostringstream oss;
                                oss << "dataAscan_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
                                ascanFilename = oss.str();
                                ascanCsvFile = make_unique<ofstream>(ascanFilename, std::ios::out | std::ios::app);
                                if (!ascanCsvFile->is_open()) {
                                    cerr << "Impossible d'ouvrir le fichier " << ascanFilename << endl;
                                }
                            }
                            if (ascanCsvFile && ascanCsvFile->is_open()) {
                                for (size_t i = 0; i < avgAscan.size(); i++) {
                                    *ascanCsvFile << avgAscan[i] << (i < avgAscan.size()-1 ? "," : "\n");
                                }
                                ascanCsvFile->flush();
                            }
                            
                            // Calcul de la FFT sur les données ascan moyennées
                            int N = avgAscan.size();
                            // Allocation des tableaux pour FFTW
                            double* fft_in = fftw_alloc_real(N);
                            fftw_complex* fft_out = fftw_alloc_complex(N/2 + 1);
                            for (int i = 0; i < N; i++) {
                                fft_in[i] = avgAscan[i];
                            }
                            fftw_plan plan = fftw_plan_dft_r2c_1d(N, fft_in, fft_out, FFTW_ESTIMATE);
                            fftw_execute(plan);
                            
                            // Création du fichier CSV pour les résultats FFT
                            string fftFilename;
                            {
                                auto t = std::time(nullptr);
                                std::tm tm = *std::localtime(&t);
                                std::ostringstream oss;
                                oss << "dataFFT_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
                                fftFilename = oss.str();
                            }
                            std::ofstream fftFile(fftFilename, std::ios::out);
                            if (!fftFile.is_open()) {
                                cerr << "Impossible d'ouvrir le fichier " << fftFilename << endl;
                            } 
                            else {
                                // En-tête : fréquence et magnitude
                                fftFile << "Frequency (Hz),Magnitude\n";
                                double sampleRate = 125e6; // 125 MHz
                                int nBins = N/2 + 1;
                                for (int i = 0; i < nBins; i++) {
                                    double freq = i * sampleRate / N;
                                    double magnitude = sqrt(fft_out[i][0]*fft_out[i][0] + fft_out[i][1]*fft_out[i][1]);
                                    fftFile << freq << "," << magnitude << "\n";
                                }
                                fftFile.flush();
                                fftFile.close();
                                cout << "FFT enregistrée dans " << fftFilename << endl;
                            }
                            
                            // Libération des ressources FFTW
                            fftw_destroy_plan(plan);
                            fftw_free(fft_in);
                            fftw_free(fft_out);
                        }
                    }
                    
                    // Réinitialiser les accumulateurs pour le prochain lot
                    messageCounter = 0;
                    fill(sumValues.begin(), sumValues.end(), 0.0);
                    if(ascanInitialized) {
                        fill(sumAscan.begin(), sumAscan.end(), 0.0);
                    }
                }
            }
            catch (const std::exception & e) {
                cerr << "Erreur lors du parsing du JSON: " << e.what() << endl;
            }
        }

        // Fermeture propre
        if (cli.is_connected()) {
            cout << "\nShutting down and disconnecting from the MQTT server..." << flush;
            cli.stop_consuming();
            cli.disconnect()->wait();
            cout << "OK" << endl;
        }
        else {
            cout << "\nClient was disconnected" << endl;
        }
    }
    catch (const mqtt::exception& exc) {
        cerr << "\n  " << exc << endl;
        return 1;
    }
    
    if (csvFile && csvFile->is_open()) {
        csvFile->close();
    }
    if (ascanCsvFile && ascanCsvFile->is_open()) {
        ascanCsvFile->close();
    }
    return 0;
}
