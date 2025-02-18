
/*******************************************************************************
 * Copyright (c) 2013-2024 Frank Pagliughi <fpagliughi@mindspring.com>
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v20.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Frank Pagliughi - initial implementation and documentation
 *******************************************************************************/

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <memory>
#include "mqtt/async_client.h"
#include "/home/kco/json/single_include/nlohmann/json.hpp"  // Bibliothèque nlohmann JSON

using json = nlohmann::json;
using namespace std;

const string DFLT_SERVER_URI{"mqtt://192.168.1.73:1883"};
const string CLIENT_ID{"PahoCppAsyncConsumeV5"};
const string TOPIC{"inspection/ascan"};
const int QOS = 1;
const int DEFAULT_AVERAGE_COUNT = 1;

int main(int argc, char* argv[])
{
    // L'adresse du broker peut être passée en argument (sinon adresse par défaut)
    auto serverURI = (argc > 1) ? string{argv[1]} : DFLT_SERVER_URI;

    // Le nombre de messages à accumuler pour la moyenne (par défaut 1 ou fourni en argument)
    int averageCount = DEFAULT_AVERAGE_COUNT;
    if(argc > 2) {
        try {
            averageCount = stoi(argv[2]);
            if(averageCount < 1) {
                cerr << "Le nombre de messages pour la moyenne doit être >= 1. Utilisation de la valeur par défaut (" 
                     << DEFAULT_AVERAGE_COUNT << ")." << endl;
                averageCount = DEFAULT_AVERAGE_COUNT;
            }
        }
        catch(...) {
            cerr << "Argument invalide pour le nombre de messages. Utilisation de la valeur par défaut (" 
                 << DEFAULT_AVERAGE_COUNT << ")." << endl;
            averageCount = DEFAULT_AVERAGE_COUNT;
        }
    }

    // Nous ne créons pas le fichier CSV tout de suite, il sera créé seulement s'il y a des données à enregistrer.
    std::unique_ptr<std::ofstream> csvFile;
    bool headerWritten = false;
    // Nom du fichier (sera défini lors de la première écriture)
    string filename;

    mqtt::async_client cli(serverURI, CLIENT_ID);

    auto connOpts = mqtt::connect_options_builder::v5()
                        .clean_start(false)
                        .properties({{mqtt::property::SESSION_EXPIRY_INTERVAL, 604800}})
                        .finalize();

    // Variables pour accumuler les valeurs sur plusieurs messages
    vector<double> sumValues(4, 0.0);
    int messageCounter = 0;
    vector<string> headerNames;  // Pour stocker les noms (champ "name") de chaque mesure

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
        cout << "\nWaiting for messages on topic: '" << TOPIC << "'" << endl;
        cout << "La moyenne de " << averageCount << " message(s) sera écrite en CSV." << endl;

        while (true) {
            auto msg = cli.consume_message();
            if (!msg)
                break;

            string payload = msg->to_string();
            
            try {
                // Parser le JSON reçu
                json j = json::parse(payload);
                // On suppose qu'il y a 4 mesures: measurement.1 à measurement.4
                for (int i = 1; i <= 4; i++) {
                    string key = "measurement." + to_string(i);
                    if(j.contains(key)) {
                        json meas = j[key];
                        // Récupérer le nom de la mesure lors de la première accumulation
                        if(headerNames.size() < 4) {
                            string name = meas.value("name", "col" + to_string(i));
                            headerNames.push_back(name);
                        }
                        double value = meas.value("value", 0.0);
                        sumValues[i-1] += value;
                    }
                }
                messageCounter++;

                // Lorsque le nombre de messages accumulés atteint 'averageCount', calculer la moyenne et écrire dans le CSV
                if(messageCounter == averageCount) {
                    vector<double> avgValues(4, 0.0);
                    for (int i = 0; i < 4; i++) {
                        avgValues[i] = sumValues[i] / messageCounter;
                    }
                    
                    // Ouvrir le fichier CSV lors de la première écriture
                    if (!csvFile) {
                        // Génération du nom du fichier CSV incluant la date et l'heure (ex: data_YYYYMMDD_HHMMSS.csv)
                        auto t = std::time(nullptr);
                        std::tm tm = *std::localtime(&t);
                        std::ostringstream oss;
                        oss << "data_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
                        filename = oss.str();
                        csvFile = make_unique<ofstream>(filename, std::ios::out | std::ios::app);
                        if (!csvFile->is_open()) {
                            cerr << "Impossible d'ouvrir le fichier " << filename << endl;
                            return 1;
                        }
                    }

                    // Écriture de l'en-tête si nécessaire
                    if (!headerWritten) {
                        for (size_t i = 0; i < headerNames.size(); i++) {
                            *csvFile << headerNames[i] << (i < headerNames.size()-1 ? "," : "\n");
                        }
                        headerWritten = true;
                    }
                    // Écriture de la ligne contenant les moyennes calculées
                    for (size_t i = 0; i < avgValues.size(); i++) {
                        *csvFile << avgValues[i] << (i < avgValues.size()-1 ? "," : "\n");
                    }
                    csvFile->flush();
                    cout << ".";
                    cout.flush();
                    // Réinitialiser le compteur et les sommes pour le prochain lot
                    messageCounter = 0;
                    fill(sumValues.begin(), sumValues.end(), 0.0);
                }
            }
            catch (const std::exception & e) {
                cerr << "Erreur lors du parsing du JSON: " << e.what() << endl;
            }
        }

        // En cas de lot partiel (nombre de messages accumulés inférieur à averageCount), écrire la moyenne partielle
        if(messageCounter > 0) {
            vector<double> avgValues(4, 0.0);
            for (int i = 0; i < 4; i++) {
                avgValues[i] = sumValues[i] / messageCounter;
            }
            // Ouvrir le fichier CSV si ce n'est pas déjà fait
            if (!csvFile) {
                auto t = std::time(nullptr);
                std::tm tm = *std::localtime(&t);
                std::ostringstream oss;
                oss << "data_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
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
            cout << ".";
            cout.flush();
        }

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
    return 0;
}
