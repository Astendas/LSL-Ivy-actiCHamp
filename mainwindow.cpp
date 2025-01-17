/*#define _CRTDBG_MAP_ALLOC  
#include <stdlib.h>  
#include <crtdbg.h> */ 

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "Amplifier_LIB.h"
#include <QtCore/QtCore>

#include <iostream>
#include <sstream>

#if _WIN64
#define BITDEPTH "64"
#else
#define BITDEPTH "32"
#endif

const int m_pnBaseSamplingRates[] = {10000,50000,100000};
const int m_pnSubSampleDivisors[] = { 1,2,5,10,20,50,100 };

void Transpose(const std::vector<std::vector<double> > &in, std::vector<std::vector<double> > &out);

#define APPNAME "actiCHamp LSL Connector " << BITDEPTH << "-bit"
#define LIBVERSIONSTREAM(version) version.Major << "." << version.Minor << "." << version.Build << "." << version.Revision
#define LSLVERSIONSTREAM(version) (version/100) << "." << (version%100)
#define APPVERSIONSTREAM(version) version.Major << "." << version.Minor << "." << version.Bugfix

MainWindow::MainWindow(QWidget *parent, const char* config_file): QMainWindow(parent),ui(new Ui::MainWindow)
{
	m_AppVersion.Major = 1;
	m_AppVersion.Minor = 15;
	m_AppVersion.Bugfix = 2;

	ui->setupUi(this);
	m_bUseActiveShield = false;
	m_bOverrideAutoUpdate = false;

	// make GUI connections
	QObject::connect(ui->actionQuit, SIGNAL(triggered()), this, SLOT(close()));
	QObject::connect(ui->linkButton, SIGNAL(clicked()), this, SLOT(Link()));
	QObject::connect(ui->actionLoad_Configuration, SIGNAL(triggered()), this, SLOT(LoadConfigDialog()));
	QObject::connect(ui->actionSave_Configuration, SIGNAL(triggered()), this, SLOT(SaveConfigDialog()));
	QObject::connect(ui->baseSamplingRate, SIGNAL(currentIndexChanged(int)), this, SLOT(SetSamplingRate()));
	QObject::connect(ui->subSampleDivisor, SIGNAL(currentIndexChanged(int)), this, SLOT(SetSamplingRate()));
	QObject::connect(ui->actionVersions, SIGNAL(triggered()), this, SLOT(VersionsDialog()));
	QObject::connect(ui->refreshDevices, SIGNAL(clicked()), this, SLOT(RefreshDevices()));
	QObject::connect(ui->eegChannelCount, SIGNAL(valueChanged(int)), this, SLOT(UpdateChannelLabelsEEG(int)));
	QObject::connect(ui->auxChannelCount, SIGNAL(valueChanged(int)), this, SLOT(UpdateChannelLabelsGUI(int)));
	QObject::connect(ui->availableDevices, SIGNAL(currentIndexChanged(int)), this, SLOT(ChooseDevice(int)));
	QObject::connect(ui->rbDefault, SIGNAL(clicked(bool)), this, SLOT(RadioButtonBehavior(bool)));
	QObject::connect(ui->rbMirror, SIGNAL(clicked(bool)), this, SLOT(RadioButtonBehavior(bool)));
	QObject::connect(this,SIGNAL(send(QString)),this,SLOT(sendMsg(QString)),Qt::QueuedConnection);
	SetSamplingRate();
	QString cfgfilepath = find_config_file(config_file);

	//ivy config
	ivyqt = new IvyQt("IvyQt_EEG", "IvyQt_EEG Ready\x03", this);
	DWORD outBufLen = 1 << 19;
	PIP_ADAPTER_ADDRESSES ifaddrs = (IP_ADAPTER_ADDRESSES *) new char[outBufLen];
	ULONG RetVal = GetAdaptersAddresses(AF_UNSPEC,
		GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, ifaddrs,
		&outBufLen);

	if (RetVal == NO_ERROR) {
		for (PIP_ADAPTER_ADDRESSES addr = ifaddrs; addr != 0; addr = addr->Next) {
			// Interface isn't up or doesn't support multicast? Skip it.
			if (addr->OperStatus != IfOperStatusUp) continue;
			if (addr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
			if (addr->NoMulticast) continue;

			// Find the first IPv4 address
			if (addr->Ipv4Enabled) {
				for (IP_ADAPTER_UNICAST_ADDRESS_LH *uaddr = addr->FirstUnicastAddress; uaddr != 0; uaddr = uaddr->Next) {
					if (uaddr->Address.lpSockaddr->sa_family != AF_INET) continue;
					DWORD ip, subnet_mask;
					char str_buffer[INET_ADDRSTRLEN] = {0};
					SOCKADDR_IN* ipv4 = reinterpret_cast<SOCKADDR_IN*>(uaddr->Address.lpSockaddr);

					inet_ntop(AF_INET, &(ipv4->sin_addr), str_buffer, INET_ADDRSTRLEN);
					inet_pton(AF_INET, str_buffer, &ip);
					ConvertLengthToIpv4Mask(uaddr->OnLinkPrefixLength,&subnet_mask);

					DWORD broadcast_ip = (ip | ~subnet_mask);
					char broadcast_ip_str[16];
					inet_ntop(AF_INET, &broadcast_ip, broadcast_ip_str, 16);		
					//starting ivy on LAN
					setWindowTitle("Launching Ivy Server");
					ivyqt->start(broadcast_ip_str, 2010);
					setWindowTitle(broadcast_ip_str);
					goto ivy_initialized; //to break out of 2 loops. no it isn't bad practice in this precise case
				}
				
			}
		}
	}
	ivy_initialized:
    free(ifaddrs);


	LoadConfig(cfgfilepath);
}
void MainWindow::sendMsg(QString str){
	ivyqt->send(str);
}

QString MainWindow::find_config_file(const char* filename) {
	if (filename) {
		QString qfilename(filename);
		if (!QFileInfo::exists(qfilename))
			QMessageBox(QMessageBox::Warning, "Config file not found",
				QStringLiteral("The file '%1' doesn't exist").arg(qfilename), QMessageBox::Ok,
				this);
		else
			return qfilename;
	}
	QFileInfo exeInfo(QCoreApplication::applicationFilePath());
	QString defaultCfgFilename(exeInfo.completeBaseName() + ".cfg");
	qInfo() << defaultCfgFilename;
	QStringList cfgpaths;
	cfgpaths << QDir::currentPath()
		<< QStandardPaths::standardLocations(QStandardPaths::ConfigLocation) << exeInfo.path();
	for (auto path : cfgpaths) {
		QString cfgfilepath = path + QDir::separator() + defaultCfgFilename;
		if (QFileInfo::exists(cfgfilepath)) return cfgfilepath;
	}
	QMessageBox::warning(this, "No config file not found",
		QStringLiteral("No default config file could be found"), "Continue with default config");
	return "";
}

void MainWindow::VersionsDialog()
{
	t_VersionNumber libVersion;
	GetLibraryVersion(&libVersion);
	int32_t lslProtocolVersion = lsl::protocol_version();
	int32_t lslLibVersion = lsl::library_version();
	std::stringstream ss;
	ss << 
		"Amplifier_LIB: " << LIBVERSIONSTREAM(libVersion) << "\n" <<
		"lsl protocol: " << LSLVERSIONSTREAM(lslProtocolVersion) << "\n" <<
		"liblsl: " << LSLVERSIONSTREAM(lslLibVersion) << "\n" <<
		"App: " << APPVERSIONSTREAM(m_AppVersion) << ", " << BITDEPTH << "-bit";
	QMessageBox::information(this, "About", ss.str().c_str(), QMessageBox::Ok);
}

void MainWindow::SetSamplingRate()
{
	int nNum = m_pnBaseSamplingRates[ui->baseSamplingRate->currentIndex()];
	int nDenom = m_pnSubSampleDivisors[ui->subSampleDivisor->currentIndex()];
	std::string sSR = std::to_string(nNum / nDenom);
	ui->nominalSamplingRate->setText(sSR.c_str());
	m_nSamplingRate = nNum / nDenom;
	//setMinChunk();
}

void MainWindow::LoadConfigDialog() 
{
	QString filename = QFileDialog::getOpenFileName(this,"Load Configuration File","","Configuration Files (*.cfg)");
	if (!filename.isEmpty())
		LoadConfig(filename);
}

void MainWindow::SaveConfigDialog() 
{
	QString filename = QFileDialog::getSaveFileName(this, "Save Configuration File", "", "Configuration Files (*.cfg)");
	if (!filename.isEmpty())
		SaveConfig(filename);
}

void MainWindow::closeEvent(QCloseEvent *ev) 
{
	if (m_ptReadThread)
		ev->ignore();

}

int getBaseSamplingRateIndex(int nBaseSamplingRate)
{
	switch (nBaseSamplingRate)
	{
	case 10000:
		return 0;
	case 50000:
		return 1;
	case 100000:
		return 2;
	}
	return 0;
}

int getSubSampleDivisorIndex(int nSubSampleDivisor)
{
	switch (nSubSampleDivisor)
	{
	case 1:
		return 0;
	case 2:
		return 1;
	case 5:
		return 2;
	case 10:
		return 3;
	case 20:
		return 4;
	case 50:
		return 5;
	case 100:
		return 6;
	}
	return 0;
}

void MainWindow::RefreshDevices()
{
	ui->availableDevices->blockSignals(true);
	QStringList qsl;
	std::vector<std::pair<std::string, int>> ampData;
	this->setCursor(Qt::WaitCursor);
	this->setWindowTitle("Searching for Devices...");
	try
	{
		m_LibTalker.enumerate(ampData, ui->useSim->checkState() == Qt::Checked);
	}
	catch (std::exception & e)
	{
		QMessageBox::critical(this, "Error", (std::string("Could not locate actiCHamp device(s): ") += e.what()).c_str(), QMessageBox::Ok);
	}
	this->setCursor(Qt::ArrowCursor);
	if (!m_psAmpSns.empty())m_psAmpSns.clear();
	if (!m_pnUsableChannelsByDevice.empty()) m_pnUsableChannelsByDevice.clear();
	if (!ampData.empty()) {
		ui->availableDevices->clear();
		std::stringstream ss;
		int i = 0;
		for (std::vector<std::pair<std::string, int>>::iterator it = ampData.begin(); it != ampData.end(); ++it) {
			ss.clear();
			ss << it->first << " (" << it->second << ")";
			std::cout << it->first << std::endl;
			auto x = ss.str(); // oh, c++...
			qsl << QString(ss.str().c_str()); // oh, Qt...
			m_psAmpSns.push_back(it->first);
			m_pnUsableChannelsByDevice.push_back(it->second);
		}
		ui->availableDevices->addItems(qsl);
		ChooseDevice(0);
	}
	this->setWindowTitle("actiCHamp Connector");
	ui->availableDevices->blockSignals(true);
}

void MainWindow::UpdateChannelLabelsEEG(int n)
{

	if (m_bOverrideAutoUpdate)return;
	UpdateChannelLabels();
}

void MainWindow::UpdateChannelLabelsGUI(int n)
{

	if (m_bOverrideAutoUpdate)return;
	UpdateChannelLabels();
}


void MainWindow::UpdateChannelLabels()
{
	if (!ui->overwriteChannelLabels->isChecked())return;
	
	int nEeg = ui->eegChannelCount->value();
	int nAux = ui->auxChannelCount->value();

	std::string str;
	std::vector<std::string> psEEGChannelLabels;
	std::istringstream iss(ui->channelLabels->toPlainText().toStdString());
	int nEegChannelCount;
	while (std::getline(iss, str, '\n'))
		psEEGChannelLabels.push_back(str);
	nEegChannelCount = ui->eegChannelCount->value();

	for (auto it = psEEGChannelLabels.begin(); it != psEEGChannelLabels.end();)
	{
		if ((*(it)).find("AUX")!=std::string::npos)
			it = psEEGChannelLabels.erase(it);
		else
			++it;
	}

	while (psEEGChannelLabels.size() > nEegChannelCount)
		psEEGChannelLabels.pop_back();
	ui->channelLabels->clear();
	for (int i = 0; i < ui->eegChannelCount->value(); i++)
	{
		if (i < psEEGChannelLabels.size())
			str = psEEGChannelLabels[i];
		else
			str = std::to_string(i + 1);
		ui->channelLabels->appendPlainText(str.c_str());
	}
	for (int i = 0; i < nAux; i++)
	{
		str = "AUX_" + std::to_string(i + 1);
		ui->channelLabels->appendPlainText(str.c_str());
	}
}

void MainWindow::ChooseDevice(int which)
{

	if (!m_psAmpSns.empty())
		ui->serialNumber->setText(QString(m_psAmpSns[which].c_str()));
	ui->eegChannelCount->setMaximum(m_pnUsableChannelsByDevice[ui->availableDevices->currentIndex()]);
}

void MainWindow::RadioButtonBehavior(bool b)
{
	if (ui->rbMirror->isChecked())
	{
		m_TriggerOutputMode = TM_MIRROR;
	}
	if (ui->rbDefault->isChecked())
	{
		m_TriggerOutputMode = TM_DEFAULT;
	}
}

void MainWindow::LoadConfig(const QString &filename) 
{
	QSettings pt(filename, QSettings::IniFormat);

	ui->serialNumber->setText(pt.value("settings/serialNumber", "(enter manually or scan for devices)").toString());
	ui->eegChannelCount->setValue(pt.value("settings/eegChannelCount", 32).toInt());
	ui->auxChannelCount->setValue(pt.value("settings/auxChannelCount", 0).toInt());
	ui->chunkSize->setValue(pt.value("settings/chunkSize", 10).toInt());
	// TODO: make this a switch statement based on actual values
	int nBaseSamplingRate = pt.value("settings/baseSamplingRate", 10000).toInt();
	ui->baseSamplingRate->setCurrentIndex(getBaseSamplingRateIndex(nBaseSamplingRate));
	int nSubSampleDivisor = pt.value("settings/subSampleDivisor", 20).toInt();
	ui->nominalSamplingRate->setText((std::to_string(nBaseSamplingRate / nSubSampleDivisor)).c_str());
	ui->subSampleDivisor->setCurrentIndex(getSubSampleDivisorIndex(nSubSampleDivisor));
	//setMinChunk();
	ui->fastDataAccess->setCheckState(pt.value("settings/useFda", false).toBool() ? Qt::Checked : Qt::Unchecked);
	ui->useSampleCtr->setCheckState(pt.value("settings/useSampleCtr", false).toBool() ? Qt::Checked : Qt::Unchecked);
	m_bUseActiveShield = pt.value("settings/activeShield", false).toBool();
	ui->overwriteChannelLabels->setCheckState(pt.value("settings/overWrite", true).toBool() ? Qt::Checked : Qt::Unchecked);
	ui->sampledMarkersEEG->setCheckState(pt.value("settings/sampledMarkersEEG", false).toBool() ? Qt::Checked : Qt::Unchecked);
	ui->unsampledMarkers->setCheckState(pt.value("settings/unsampledMarkers", true).toBool() ? Qt::Checked : Qt::Unchecked);
	t_TriggerOutputMode triggerOutputMode = (t_TriggerOutputMode)pt.value("settings/triggerOutputMode", 0).toInt();
	if (triggerOutputMode == TM_MIRROR)ui->rbMirror->setChecked(true);
	else ui->rbDefault->setChecked(true);
	ui->channelLabels->clear();
	ui->channelLabels->setPlainText(pt.value("channels/labels").toStringList().join('\n'));
	UpdateChannelLabels();
	SetSamplingRate();

}

void MainWindow::SaveConfig(const QString& filename)
{
	QSettings pt(filename, QSettings::IniFormat);

	pt.beginGroup("settings");
	pt.setValue("serialNumber", ui->serialNumber->text());
	pt.setValue("eegChannelCount", ui->eegChannelCount->value());
	pt.setValue("auxChannelCount", ui->auxChannelCount->value());
	pt.setValue("useSampleCtr", ui->useSampleCtr->checkState() == Qt::Checked);
	pt.setValue("useFda", ui->fastDataAccess->checkState() == Qt::Checked);
	pt.setValue("chunkSize", ui->chunkSize->value());
	pt.setValue("baseSamplingRate", m_pnBaseSamplingRates[ui->baseSamplingRate->currentIndex()]);
	pt.setValue("subSampleDivisor", m_pnSubSampleDivisors[ui->subSampleDivisor->currentIndex()]);
	pt.setValue("unsampledMarkers", ui->unsampledMarkers->checkState() == Qt::Checked);
	pt.setValue("sampledMarkersEEG", ui->sampledMarkersEEG->checkState() == Qt::Checked);
	pt.setValue("overWrite", ui->overwriteChannelLabels->checkState() == Qt::Checked);
	pt.setValue("triggerOutputMode", m_TriggerOutputMode);
	pt.endGroup();
	pt.beginGroup("channels");
	pt.setValue("labels", ui->channelLabels->toPlainText().split('\n'));
	pt.endGroup();
}

void MainWindow::AmpSetup(t_AmpConfiguration ampConfiguration)
{
	m_LibTalker.setUseTriggers(ampConfiguration.m_bSampledMarkersEEG || ampConfiguration.m_bUnsampledMarkers);
	m_LibTalker.setExcludeTriggersFromOutput(!ui->sampledMarkersEEG->isChecked());
	m_LibTalker.setRequestedEEGChannelCnt(ampConfiguration.m_psEegChannelLabels.size());
	m_LibTalker.setRequestedAuxChannelCnt(ampConfiguration.m_psAuxChannelLabels.size());
	m_LibTalker.setBaseSamplingRate((float)ampConfiguration.m_nBaseSamplingRate);
	m_LibTalker.setSubSampleDivisor((float)ampConfiguration.m_nSubSampleDivisor);
	m_LibTalker.setUseFDA(ampConfiguration.m_bFDA);
	m_LibTalker.setUseSampleCtr(ampConfiguration.m_bUseSampleCtr);
	m_LibTalker.setUseActiveShield(false);
	m_LibTalker.Setup();
}

void MainWindow::ToggleGUIEnabled(bool bEnabled)
{
	if (bEnabled)
	{
		ui->channelLabels->setEnabled(true);
		ui->overwriteChannelLabels->setEnabled(true);
		ui->deviceGroup->setEnabled(true);
		ui->scanGroup->setEnabled(true);
		ui->channelGroup->setEnabled(true);
		ui->linkButton->setEnabled(true);
		ui->linkButton->setText("Link");
	}
	else
	{
		ui->channelLabels->setEnabled(false);
		ui->overwriteChannelLabels->setEnabled(false);
		ui->deviceGroup->setEnabled(false);
		ui->scanGroup->setEnabled(false);
		ui->channelGroup->setEnabled(false);
		ui->linkButton->setEnabled(true);
		ui->linkButton->setText("Unlink");
	}
}

void MainWindow::Link() 
{
	bool bSuccess = true;
	if (m_ptReadThread) 
	{
		try 
		{
			m_bStop = true;
			m_ptReadThread->join();
			m_ptReadThread.reset();
			m_LibTalker.setOutTriggerMode(TriggerOutputMode::TM_DEFAULT);
		} 
		catch(std::exception &e) 
		{
			QMessageBox::critical(this,"Error",(std::string("Could not stop the background processing: ")+=e.what()).c_str(),QMessageBox::Ok);
			return;
		}
		ToggleGUIEnabled(true);
		ui->linkButton->setText("Link");
	} 
	else 
	{
		try 
		{
			// get the UI parameters...
			t_AmpConfiguration ampConfiguration;
			ampConfiguration.m_sSerialNumber = ui->serialNumber->text().toStdString();
			ampConfiguration.m_bUseActiveShield = m_bUseActiveShield;
			ampConfiguration.m_bFDA = ui->fastDataAccess->checkState() == Qt::Checked;
			ampConfiguration.m_bUseSampleCtr = ui->useSampleCtr->checkState() == Qt::Checked;
			ampConfiguration.m_nChunkSize = ui->chunkSize->value();
			ampConfiguration.m_nBaseSamplingRate = ui->baseSamplingRate->currentText().toInt();
			ampConfiguration.m_nSubSampleDivisor = ui->subSampleDivisor->currentText().toInt();
			ampConfiguration.m_bUseSim = ui->useSim->checkState() == Qt::Checked;
			SetSamplingRate();
			std::vector<std::string> psChannelLabels;
			std::string str;
			std::istringstream iss(ui->channelLabels->toPlainText().toStdString());
			while (std::getline(iss, str, '\n'))
				psChannelLabels.push_back(str);
			std::vector<std::string> psEegChannelLabels;
			std::vector<std::string> psAuxChannelLabels;
			int i = 0;
			for (std::vector<std::string>::iterator it = psChannelLabels.begin();
				it < psChannelLabels.end();
				++it)
			{
				if (i < ui->eegChannelCount->value())
					psEegChannelLabels.push_back(*it);
				else
					psAuxChannelLabels.push_back(*it);
				i++;
			}
			ampConfiguration.m_psEegChannelLabels = psEegChannelLabels;
			ampConfiguration.m_psAuxChannelLabels = psAuxChannelLabels;
			ampConfiguration.m_bUnsampledMarkers = ui->unsampledMarkers->checkState() == Qt::Checked;
			ampConfiguration.m_bSampledMarkersEEG = ui->sampledMarkersEEG->checkState() == Qt::Checked;
			if (ampConfiguration.m_psEegChannelLabels.size() > 32 && (ampConfiguration.m_nBaseSamplingRate / ampConfiguration.m_nSubSampleDivisor == 100000))
			{
				QMessageBox::critical(this, "Error", "Can not initialize the actiCHamp. A Sampling Rate of 100kHz is limited to 32 channels of EEG.", QMessageBox::Ok);
				return;
			}
			if (ampConfiguration.m_psEegChannelLabels.size() > 64 && (ampConfiguration.m_nBaseSamplingRate / ampConfiguration.m_nSubSampleDivisor == 50000))
			{
				QMessageBox::critical(this, "Error", "Can not initialize the actiCHamp. A Sampling Rate of 50kHz is limited to 64 channels of EEG.", QMessageBox::Ok);
				return;
			}
			this->setWindowTitle(QString("Attempting to connect"));
			this->setCursor(Qt::WaitCursor);
			m_LibTalker.Connect(ampConfiguration.m_sSerialNumber, ampConfiguration.m_bUseSim);
			if(!m_LibTalker.setOutTriggerMode(m_TriggerOutputMode))
				ui->rbDefault->setChecked(true);
			AmpSetup(ampConfiguration);
			this->setCursor(Qt::ArrowCursor);
			this->setWindowTitle("actiCHamp Connector");
			
			// start reader thread
			m_bStop = false;
			m_ptReadThread.reset(new std::thread(&MainWindow::ReadThread,this, ampConfiguration));
		}
		catch(std::exception &e) 
		{

			std::string msg = e.what();
			QMessageBox::critical(this,"Error",("Could not initialize the actiCHamp: " + msg).c_str(),QMessageBox::Ok);	
			this->setCursor(Qt::ArrowCursor);
			ToggleGUIEnabled(true);
			return;
		}
		ToggleGUIEnabled(false);
		ui->linkButton->setText("Unlink");
	}
}



// background data reader thread
void MainWindow::ReadThread(t_AmpConfiguration ampConfiguration) 
{
	int res = SetPriorityClass(GetCurrentThread(), HIGH_PRIORITY_CLASS);
	//int nEnabledChannelCnt = m_nEEGChannelCount + ((m_bUseAUX) ? 8 : 0);
	int nChannelLabelCnt = ampConfiguration.m_psEegChannelLabels.size() + ampConfiguration.m_psAuxChannelLabels.size();
	int nExtraEEGChannelCnt = 0;
	//int nTriggerIdx = nChannelLabelCnt;
	//int nSampleCounterIdx = nTriggerIdx + 1;
	if (ampConfiguration.m_bSampledMarkersEEG) nExtraEEGChannelCnt+=1;
	if (ampConfiguration.m_bUseSampleCtr) nExtraEEGChannelCnt += 1;

	try
	{
		// for debugging:
		bool bFDA = m_LibTalker.CheckFDA();

		// setup LSL
		lsl::stream_outlet* poutMarkerOutlet = NULL;
		std::cout << std::string("actiCHamp" + m_LibTalker.getSerialNumber());
		std::cout << "EEG";
		std::cout << nChannelLabelCnt + nExtraEEGChannelCnt;
		std::cout << (double)m_nSamplingRate;
		std::cout << lsl::cf_float32;
		std::cout << m_LibTalker.getSerialNumber();
		lsl::stream_info infoData("actiCHamp-" + m_LibTalker.getSerialNumber(),
			"EEG",
			nChannelLabelCnt + nExtraEEGChannelCnt,
			(double)m_nSamplingRate,
			lsl::cf_float32,
			m_LibTalker.getSerialNumber());


		lsl::xml_element channels = infoData.desc().append_child("channels");
		for (int i = 0; i < ampConfiguration.m_psEegChannelLabels.size(); i++)
		{
			channels.append_child("channel")
				.append_child_value("label", ampConfiguration.m_psEegChannelLabels[i].c_str())
				.append_child_value("type", "EEG")
				.append_child_value("unit", "microvolts");
		}
		for (int i = 0; i < ampConfiguration.m_psAuxChannelLabels.size(); i++)
		{
			channels.append_child("channel")
				.append_child_value("label", ampConfiguration.m_psAuxChannelLabels[i].c_str())
				.append_child_value("type", "AUX")
				.append_child_value("unit", "microvolts");
		}
		if (ampConfiguration.m_bSampledMarkersEEG)
			channels.append_child("channel")
			.append_child_value("label", "Markers")
			.append_child_value("type", "Markers")
			.append_child_value("unit", "");

		if (ampConfiguration.m_bUseSampleCtr)
			channels.append_child("channel")
			.append_child_value("label", "SampleCounter")
			.append_child_value("type", "SampleCounter")
			.append_child_value("unit", "");

		infoData.desc().append_child("acquisition")
			.append_child_value("manufacturer", "Brain Products");

		t_VersionNumber libVersion;
		GetLibraryVersion(&libVersion);
		int32_t lslProtocolVersion = lsl::protocol_version();
		int32_t lslLibVersion = lsl::library_version();
		std::stringstream ssLib;
		ssLib << LIBVERSIONSTREAM(libVersion);
		std::stringstream ssProt;
		ssProt << LSLVERSIONSTREAM(lslProtocolVersion);
		std::stringstream ssLSL;
		ssLSL << LSLVERSIONSTREAM(lslLibVersion);
		std::stringstream ssApp;
		ssApp << APPVERSIONSTREAM(m_AppVersion);

		infoData.desc().append_child("versions")
			.append_child_value("Amplifier_LIB", ssLib.str())
			.append_child_value("lsl_protocol", ssProt.str())
			.append_child_value("liblsl", ssLSL.str())
			.append_child_value("App", ssApp.str());

		lsl::stream_outlet outData(infoData);

		lsl::stream_info infoMarkers("actiCHampMarkers-" + m_LibTalker.getSerialNumber(),
			"Markers",
			1,
			0,
			lsl::cf_string,
			m_LibTalker.getSerialNumber());

		if (ampConfiguration.m_bUnsampledMarkers)
		{
			infoMarkers.desc().append_child("channels")
				.append_child("channel")
				.append_child_value("label", "Markers")
				.append_child_value("type", "Markers")
				.append_child_value("unit", "");

			infoMarkers.desc().append_child("acquisition")
				.append_child_value("manufacturer", "Brain Products");

			infoMarkers.desc().append_child("versions")
				.append_child_value("Amplifier_LIB", ssLib.str())
				.append_child_value("lsl_protocol", ssProt.str())
				.append_child_value("liblsl", ssLSL.str())
				.append_child_value("App", ssApp.str());

			poutMarkerOutlet = new lsl::stream_outlet(infoMarkers);

		}
		std::vector<float> vfDataBufferMultiplexed;
		std::vector<int> vnTriggers;
		int nMarker;
		int nPreviousMarker = -1;
		int32_t nBufferSize = ampConfiguration.m_nChunkSize * m_LibTalker.getSampleSize();
		BYTE* buffer = NULL;
		buffer = new BYTE[nBufferSize];
		int64_t nSampleCnt;
		double dNow;
		int nSamplesPulled = 0;
		dNow = lsl::local_clock();
		vfDataBufferMultiplexed.resize(ampConfiguration.m_nChunkSize * (nChannelLabelCnt + nExtraEEGChannelCnt));
		vnTriggers.resize(ampConfiguration.m_nChunkSize);

		//initial ivy message
		ivyqt->bindMessage("EEG_Init_Request",[=](Peer*, QStringList){
		QString init_message_ivy="EEG_Init ";
		init_message_ivy.append(QString::number(nChannelLabelCnt));
		init_message_ivy.append(";");
		init_message_ivy.append(QString::number((int)m_nSamplingRate));
        emit send(init_message_ivy);});



		while (!m_bStop)
		{

			nSampleCnt = m_LibTalker.PullAmpData(buffer, nBufferSize, vfDataBufferMultiplexed, vnTriggers);
			if (nSampleCnt <= 0) {
				// CPU saver, this is ok even at higher sampling rates
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			nSamplesPulled += nSampleCnt;

			if (ampConfiguration.m_bUnsampledMarkers)
			{
				for (int s = 0; s < nSampleCnt; s++)
				{
					nMarker = (int)vnTriggers[s];
					if (nMarker != nPreviousMarker)
					{
						std::string sMarker = std::to_string(nMarker);
						poutMarkerOutlet->push_sample(&sMarker, dNow + (s + 1 - nSampleCnt) / m_nSamplingRate);
					}
					nPreviousMarker = nMarker;
				}
			}
			//Ivy message sending
			for (int i=0;i<nSampleCnt;i++){
				QString m_message_ivy="EEG_Data ";
				m_message_ivy.append(QString::number(dNow,'g',8));
				m_message_ivy.append(" ");
				for(int k=0;k<nChannelLabelCnt;k++){
					m_message_ivy.append(QString::number(((float)vfDataBufferMultiplexed[i*(nChannelLabelCnt + nExtraEEGChannelCnt) + k]),'g',6));
					m_message_ivy.append(";");
				}
				emit send(m_message_ivy.toUtf8());
			}
			outData.push_chunk_multiplexed(vfDataBufferMultiplexed, dNow);
			vfDataBufferMultiplexed.clear();
			vnTriggers.clear();
			dNow = lsl::local_clock();
			nSamplesPulled = 0;

			
		}
		if (poutMarkerOutlet != NULL)
			delete(poutMarkerOutlet);
	}
	catch (std::exception & e)
	{
		std::string msg = e.what();
	}

}

MainWindow::~MainWindow() {
	delete ui;
}

