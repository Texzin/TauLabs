/**
 ******************************************************************************
 *
 * @file       il2simulator.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief
 * @see        The GNU Public License (GPL) Version 3
 * @defgroup   hitlplugin
 * @{
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * Description of DeviceLink Protocol:
 * A Request is initiated with R/ followed by id's of to be requested settings
 * even id's indicate read only values, odd are write only
 * (usually id =get value id+1= set - for same setting)
 * id's are separated by /
 * requests can contain values to set, or to select a subsystem
 * values are separated by \
 * example: R/30/48/64\0/64\1/
 * request read only settings 30,48 and 64 with parameters 0 and 1
 * the answer consists of an A followed by id value pairs in the same format
 * example: A/30\0/48\0/64\0\22/64\1\102/
 *
 * A full protocol description as well as a list of ID's and their meanings
 * can be found shipped with IL2 in the file DeviceLink.txt
 *
 * id's used in this file:
 * 30: IAS in km/h (float)
 * 32: vario in m/s (float)
 * 38: angular speed °/s (float) (which direction? azimuth?)
 * 40: barometric alt in m (float)
 * 42: flight course in ° (0-360) (float)
 * 46: roll angle in ° (-180 - 180) (floatniguration)
 * 48: pitch angle in ° (-90 - 90) (float)
 * 80/81: engine power  (-1.0 (0%) - 1.0 (100%)) (float)
 * 84/85: aileron servo (-1.0 - 1.0) (float)
 * 86/87: elevator servo (-1.0 - 1.0) (float)
 * 88/89: rudder servo (-1.0 - 1.0) (float)
 *
 * IL2 currently offers no useful way of providing GPS data
 * therefore fake GPS data will be calculated using IMS
 *
 * unfortunately angular acceleration provided is very limited, too
 */


#include "il2simulator.h"
#include "extensionsystem/pluginmanager.h"
#include <coreplugin/icore.h>
#include <coreplugin/threadmanager.h>
#include <math.h>
#include <qxtlogger.h>

const float IL2Simulator::FT2M = 0.3048;
const float IL2Simulator::KT2MPS = 0.514444444;
const float IL2Simulator::MPS2KMH = 3.6;
const float IL2Simulator::KMH2MPS = (1.0/3.6);
const float IL2Simulator::INHG2KPA = 3.386;
const float IL2Simulator::RAD2DEG = (180.0/M_PI);
const float IL2Simulator::DEG2RAD = (M_PI/180.0);
const float IL2Simulator::M2DEG =  60.*1852.; // 60 miles per degree times 1852 meters per mile
const float IL2Simulator::DEG2M = (1.0/(60.*1852.));
const float IL2Simulator::AIR_CONST = 287.058; // J/(kg*K)
const float IL2Simulator::GROUNDDENSITY = 1.225; // kg/m³ ;)
const float IL2Simulator::TEMP_GROUND = (15.0 + 273.0); // 15°C in Kelvin
const float IL2Simulator::TEMP_LAPSE_RATE = -0.0065; //degrees per meter
const float IL2Simulator::AIR_CONST_FACTOR = -0.0341631947363104; //several nature constants calculated into one

IL2Simulator::IL2Simulator(const SimulatorSettings& params) :
		Simulator(params)
{

}


IL2Simulator::~IL2Simulator()
{
}

void IL2Simulator::setupUdpPorts(const QString& host, int inPort, int outPort)
{
	inSocket->connectToHost(host,inPort); // IL2
	if(!inSocket->waitForConnected())
		qxtLog->error(Name() + " cann't connect to UDP Port: " + QString::number(inPort));

}

void IL2Simulator::transmitUpdate()
{
	// Read ActuatorDesired from autopilot
	ActuatorDesired::DataFields actData = actDesired->getData();
	float ailerons = actData.Roll;
	float elevator = actData.Pitch;
	float rudder = actData.Yaw;
	float throttle = actData.Throttle*2-1.0;

	// Send update to Il2
	QString cmd;
	cmd=QString("R/30/32/40/42/46/48/81\\%1/85\\%2/87\\%3/89\\%4/")
		.arg(throttle)
		.arg(ailerons)
		.arg(elevator)
		.arg(rudder);
	QByteArray data = cmd.toAscii();
	//outSocket->write(data);
	inSocket->write(data);  // for IL2 must send to the same port as input!!!!!!!!!!!!!
}


/**
 * calculate air density from altitude
 */
float IL2Simulator::DENSITY(float alt) {
	return (GROUNDDENSITY * pow(
				((TEMP_GROUND+(TEMP_LAPSE_RATE*alt))/TEMP_GROUND),
				((AIR_CONST_FACTOR/TEMP_LAPSE_RATE)-1) )
				);
}

/**
 * calculate air pressure from altitude
 */
float IL2Simulator::PRESSURE(float alt) {
	return DENSITY(alt)*(TEMP_GROUND+(alt*TEMP_LAPSE_RATE))*AIR_CONST;

}

/**
 * calculate TAS from IAS and altitude
 */
float IL2Simulator::TAS(float IAS, float alt) {
	return (IAS*sqrt(GROUNDDENSITY/DENSITY(alt)));
}

/**
 * process data string from flight simulator
 */
void IL2Simulator::processUpdate(const QByteArray& inp)
{
	// save old flight data to calculate delta's later
	old=current;
	QString data(inp);
	// Split
	QStringList fields = data.split("/");

	// split up response string
	int t;
	for (t=0; t<fields.length(); t++) {
		QStringList values = fields[t].split("\\");
	// parse values
	if (values.length()>=2) {
		int id = values[0].toInt();
		float value = values[1].toFloat();
		switch (id) {
			case 30:
			current.ias=value * KMH2MPS;
			break;
			case 32:
			current.dZ=value;
			break;
			case 40:
			current.Z=value;
			break;
			case 42:
			current.azimuth=value;
			break;
			case 46:
			current.roll=-value;
			break;
			case 48:
			current.pitch=value;
			break;
		}
	}
	}

	// measure time
	current.dT = ((float)time->restart()) / 1000.0;
	current.T = old.T+current.dT;

	// calculate TAS from alt and IAS
	current.tas = TAS(current.ias,current.Z);

	// assume the plane actually flies straight and no wind
	// groundspeed is horizontal vector of TAS
	current.groundspeed = current.tas*cos(current.pitch*DEG2RAD);
	// x and y vector components
	current.dX = current.groundspeed*sin(current.azimuth*DEG2RAD);
	current.dY = current.groundspeed*cos(current.azimuth*DEG2RAD);

	// simple IMS - integration over time the easy way...
	current.X = old.X + (current.dX*current.dT);
	current.Y = old.Y + (current.dY*current.dT);

	// accelerations (filtered)
#define SPEED_FILTER 2
	current.ddX = ((current.dX-old.dX) + SPEED_FILTER * (old.ddX)) / (SPEED_FILTER+1);
	current.ddY = ((current.dY-old.dY) + SPEED_FILTER * (old.ddY)) / (SPEED_FILTER+1);
	current.ddZ = ((current.dZ-old.dZ) + SPEED_FILTER * (old.ddZ)) / (SPEED_FILTER+1);

#define TURN_FILTER 2
	// turn speeds (filtered)
	current.dAzimuth = ((current.azimuth-old.azimuth) + TURN_FILTER * (old.dAzimuth)) / (TURN_FILTER+1);
	current.dPitch   = ((current.pitch-old.pitch)     + TURN_FILTER * (old.dPitch))   / (TURN_FILTER+1);
	current.dRoll    = ((current.roll-old.roll)       + TURN_FILTER * (old.dRoll))    / (TURN_FILTER+1);

	// Update AltitudeActual object
	BaroAltitude::DataFields altActualData;
	memset(&altActualData, 0, sizeof(BaroAltitude::DataFields));
	altActualData.Altitude = current.Z;
	altActualData.Temperature = TEMP_GROUND + (current.Z * TEMP_LAPSE_RATE) - 273.0;
	altActualData.Pressure = PRESSURE(current.Z)/1000.0; // kpa
	altActual->setData(altActualData);

	// Update attActual object
	AttitudeActual::DataFields attActualData;
	memset(&attActualData, 0, sizeof(AttitudeActual::DataFields));
	attActualData.Roll = current.roll;
	attActualData.Pitch = current.pitch;
	attActualData.Yaw = current.azimuth;
	attActualData.q1 = 0;
	attActualData.q2 = 0;
	attActualData.q3 = 0;
	attActualData.q4 = 0;
	attActual->setData(attActualData);

	// Update positionActual objects
	PositionActual::DataFields posData;
	posData.North =  current.Y*100;
	posData.East = current.X*100;
	posData.Down = current.Z*-100;
	posActual->setData(posData);

	// Update velocityActual objects
	VelocityActual::DataFields velData;
	velData.North = current.dY*100;
	velData.East = current.dX*100;
	velData.Down = current.dZ*100;
	velActual->setData(velData);

	// Update AttitudeRaw object (filtered gyros only for now)
	AttitudeRaw::DataFields rawData;
	rawData.gyros_filtered[0] = current.dRoll;
	rawData.gyros_filtered[1] = cos(DEG2RAD * current.roll) * current.dPitch + sin(DEG2RAD * current.roll) * current.dAzimuth;
	rawData.gyros_filtered[2] = cos(DEG2RAD * current.roll) * current.dAzimuth - sin(DEG2RAD * current.roll) * current.dPitch;
	attRaw->setData(rawData);

	// Update homelocation
	HomeLocation::DataFields homeData;
	homeData.Latitude = settings.latitude.toFloat() * 10e6;
	homeData.Longitude = settings.longitude.toFloat() * 10e6;
	homeData.Altitude = 0;
	double LLA[3];
	LLA[0]=settings.latitude.toFloat();
	LLA[1]=settings.longitude.toFloat();
	LLA[2]=0;
	double ECEF[3];
	Utils::CoordinateConversions().RneFromLLA(LLA,(double(*)[3])homeData.RNE);
	Utils::CoordinateConversions().LLA2ECEF(LLA,ECEF);
	homeData.ECEF[0]=ECEF[0]*100;
	homeData.ECEF[1]=ECEF[1]*100;
	homeData.ECEF[2]=ECEF[2]*100;
	homeData.Be[0]=0;
	homeData.Be[1]=0;
	homeData.Be[2]=0;
	posHome->setData(homeData);

	// Update gps objects
        GPSPosition::DataFields gpsData;
        memset(&gpsData, 0, sizeof(GPSPosition::DataFields));
        gpsData.Altitude = current.Z;
	gpsData.Heading = current.azimuth;
	gpsData.Groundspeed = current.groundspeed;
	double NED[3];
	NED[0] = current.Y;
	NED[1] = current.X;
	NED[2] = -current.Z;
	Utils::CoordinateConversions().GetLLA(ECEF,NED,LLA);
	gpsData.Latitude = LLA[0];
	gpsData.Longitude = LLA[1];
	gpsData.Satellites = 7;
        gpsData.Status = GPSPosition::STATUS_FIX3D;
        gpsPos->setData(gpsData);

	// issue manual update
	attActual->updated();
	altActual->updated();
	posActual->updated();
	velActual->updated();
        posHome->updated();
        gpsPos->updated();
        attRaw->updated();
}

