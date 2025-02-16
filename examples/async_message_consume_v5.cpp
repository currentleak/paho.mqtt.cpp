#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "mqtt/async_client.h"
#include "/home/kco/json/single_include/nlohmann/json.hpp"  // Ajoutez la bibliothèque nlohmann JSON (https://github.com/nlohmann/json)

using json = nlohmann::json;
using namespace std;

const string DFLT_SERVER_URI{"mqtt://192.168.1.73:1883"};
const string CLIENT_ID{"PahoCppAsyncConsumeV5"};
const string TOPIC{"inspection/ascan"};
const int QOS = 1;

int main(int argc, char* argv[])
{
    auto serverURI = (argc > 1) ? string{argv[1]} : DFLT_SERVER_URI;
    mqtt::async_client cli(serverURI, CLIENT_ID);

    auto connOpts = mqtt::connect_options_builder::v5()
                        .clean_start(false)
                        .properties({{mqtt::property::SESSION_EXPIRY_INTERVAL, 604800}})
                        .finalize();

    // Ouvrir le fichier CSV en mode ajout
    std::ofstream csvFile("data.csv", std::ios::out | std::ios::app);
    bool headerWritten = false;
    // Vérifier si le fichier est vide pour écrire l'en-tête
    csvFile.seekp(0, std::ios::end);
    if(csvFile.tellp() == 0)
        headerWritten = false;
    else
        headerWritten = true;

    try {
        cli.set_connection_lost_handler([](const std::string&) {
            cout << "*** Connection Lost ***" << endl;
        });
        cli.set_disconnected_handler([](const mqtt::properties&, mqtt::ReasonCode reason) {
            cout << "*** Disconnected. Reason [0x" << hex << int{reason} << "]: " << reason << " ***" << endl;
        });

        // Démarrer la consommation pour ne pas manquer de messages
        cli.start_consuming();

        // Connexion au broker
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

        while (true) {
            auto msg = cli.consume_message();
            if (!msg)
                break;

            // Récupérer le payload sous forme de chaîne
            string payload = msg->to_string();

            try {
                // Parser le JSON reçu
                json j = json::parse(payload);
                vector<string> header;
                vector<double> values;

                // On suppose ici 4 mesures : measurement.1 à measurement.4
                for (int i = 1; i <= 4; i++) {
                    string key = "measurement." + to_string(i);
                    if(j.contains(key)) {
                        json meas = j[key];
                        string name = meas.value("name", "col" + to_string(i));
                        double value = meas.value("value", 0.0);
                        header.push_back(name);
                        values.push_back(value);
                    }
                }

                // Écriture de l'en-tête (si non encore écrit)
                if (!headerWritten) {
                    for (size_t i = 0; i < header.size(); i++) {
                        csvFile << header[i] << (i < header.size()-1 ? "," : "\n");
                    }
                    headerWritten = true;
                }
                // Écriture des données
                for (size_t i = 0; i < values.size(); i++) {
                    csvFile << values[i] << (i < values.size()-1 ? "," : "\n");
                }
                csvFile.flush();
                cout << "Message traité et écrit en CSV." << endl;
            }
            catch (const std::exception & e) {
                cerr << "Erreur lors du parsing du JSON: " << e.what() << endl;
            }
        }

        // Arrêt du consommateur et déconnexion
        if (cli.is_connected()) {
            cout << "\nShutting down and disconnecting from the MQTT server..." << flush;
            cli.stop_consuming();
            cli.disconnect()->wait();
            cout << "OK" << endl;
        } else {
            cout << "\nClient was disconnected" << endl;
        }
    }
    catch (const mqtt::exception& exc) {
        cerr << "\n  " << exc << endl;
        return 1;
    }
    
    csvFile.close();
    return 0;
}
