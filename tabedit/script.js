window.onload = function() {
    const tabEditor = document.getElementById('tabEditor');

    // Create 6 rows for each string, each with 16 columns for time positions
    for (let string = 1; string <= 6; string++) {
        for (let position = 1; position <= 16; position++) {
            const cell = document.createElement('div');
            cell.setAttribute('contenteditable', 'true'); // Makes the cell editable
            cell.setAttribute('draggable', 'true'); // Makes the cell draggable
            cell.innerHTML = '&nbsp;'; // Placeholder, could be changed to fret number

            cell.addEventListener('dragstart', function(event) {
                event.dataTransfer.setData('text', event.target.innerHTML);
            });

            cell.addEventListener('drop', function(event) {
                event.preventDefault();
                const data = event.dataTransfer.getData('text');
                event.target.innerHTML = data;
            });

            cell.addEventListener('dragover', function(event) {
                event.preventDefault(); // Necessary to allow drop
            });

            tabEditor.appendChild(cell);
        }
    }
};
