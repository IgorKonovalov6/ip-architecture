#include <iostream>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "motor.h"
using json = nlohmann::json;
int main() {
    httplib::Server svr;
    Motor robot;
    svr.Post("/commands", [](const httplib::Request& req, httplib::Response& res) {
        std::string request_body = req.body;
        if (req.get_header_value("Content-Type").find("application/json") == std::string::npos) {
            res.status = 415;
            res.set_content("Unsupported Content-Type. Expected application/json", "text/plain");
            return;
        }
        try {
            json request_json = json::parse(request_body);
            std::string name = request_json.value("command", "none");
            std::cout << "Received command: " << name << std::endl;
            if(name == "forward")
                robot.forward();
            else if(name == "left")
                robot.left();
            else if(name == "right")
                robot.right();
            else if(name == "stop")
                robot.stop();
            int id = request_json.value("id", 0);
            json response_json;
            response_json["message"] = "Command received successfully";
            response_json["received_id"] = id;
            response_json["status"] = "success";
            res.set_content(response_json.dump(), "application/json");
            res.status = 200; 
        } catch (const json::parse_error& e) {
            res.status = 400;
            res.set_content("Invalid JSON format: " + std::string(e.what()), "text/plain");
        }
    });
    std::cout << "Server listening on http://localhost:8080" << std::endl;
    std::cout << "Try: curl -X POST -H \"Content-Type: application/json\" -d '{\"command\": \"forward\", \"time\": 1, \"id\": 1}' http://localhost:8080/commands" << std::endl;    
    svr.listen("0.0.0.0", 8080);
    return 0;
}
